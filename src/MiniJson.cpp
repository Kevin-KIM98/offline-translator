#include "translator/MiniJson.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace translator {

namespace {
const Json& nullSentinel() {
    static const Json kNull;
    return kNull;
}
const std::string& emptyString() {
    static const std::string kEmpty;
    return kEmpty;
}
const Json::Array& emptyArray() {
    static const Json::Array kEmpty;
    return kEmpty;
}
const Json::Object& emptyObject() {
    static const Json::Object kEmpty;
    return kEmpty;
}
} // namespace

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const std::string& Json::asString() const { return isString() ? str_ : emptyString(); }
const Json::Array& Json::asArray() const { return isArray() ? arr_ : emptyArray(); }
const Json::Object& Json::asObject() const { return isObject() ? obj_ : emptyObject(); }

Json::Array& Json::asArray() {
    if (!isArray()) {
        type_ = Type::Array;
        arr_.clear();
    }
    return arr_;
}

Json::Object& Json::asObject() {
    if (!isObject()) {
        type_ = Type::Object;
        obj_.clear();
    }
    return obj_;
}

bool Json::contains(const std::string& key) const {
    if (!isObject()) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    if (!isObject()) return nullSentinel();
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return nullSentinel();
}

Json& Json::operator[](const std::string& key) {
    Object& o = asObject();
    for (auto& kv : o)
        if (kv.first == key) return kv.second;
    o.emplace_back(key, Json());
    return o.back().second;
}

const Json& Json::at(std::size_t i) const {
    if (!isArray() || i >= arr_.size()) return nullSentinel();
    return arr_[i];
}

std::size_t Json::size() const {
    if (isArray()) return arr_.size();
    if (isObject()) return obj_.size();
    return 0;
}

void Json::push_back(Json v) { asArray().push_back(std::move(v)); }

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct Json::Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;
    int depth = 0;

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const char* msg) {
        if (err.empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s at offset %zu", msg, i);
            err = buf;
        }
        return false;
    }

    void skipWs() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }

    bool match(const char* lit) {
        const std::size_t n = std::strlen(lit);
        if (s.compare(i, n, lit) == 0) {
            i += n;
            return true;
        }
        return false;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t& out) {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected '\"'");
        ++i;
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            const char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return fail("unterminated escape");
                const char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t cp;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
                            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                                i += 2;
                                std::uint32_t lo;
                                if (!parseHex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    return fail("invalid low surrogate");
                                }
                            } else {
                                return fail("missing low surrogate");
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        return fail("invalid escape sequence");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            } else {
                out.push_back(c);
            }
        }
    }

    bool parseNumber(Json& out) {
        const std::size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size()) return fail("truncated number");
        if (s[i] == '0') {
            ++i;
        } else if (s[i] >= '1' && s[i] <= '9') {
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        } else {
            return fail("invalid number");
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return fail("invalid fraction");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return fail("invalid exponent");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        const std::string tok = s.substr(start, i - start);
        out = Json(std::strtod(tok.c_str(), nullptr));
        return true;
    }

    bool parseValue(Json& out) {
        if (++depth > 256) return fail("nesting too deep");
        skipWs();
        if (i >= s.size()) return fail("unexpected end of input");
        const char c = s[i];
        bool ok;
        if (c == '{') {
            ++i;
            Object obj;
            skipWs();
            if (i < s.size() && s[i] == '}') {
                ++i;
                out = Json(std::move(obj));
                ok = true;
            } else {
                while (true) {
                    skipWs();
                    std::string key;
                    if (!parseString(key)) return false;
                    skipWs();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    Json v;
                    if (!parseValue(v)) return false;
                    obj.emplace_back(std::move(key), std::move(v));
                    skipWs();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}'");
                }
                out = Json(std::move(obj));
                ok = true;
            }
        } else if (c == '[') {
            ++i;
            Array arr;
            skipWs();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                while (true) {
                    Json v;
                    if (!parseValue(v)) return false;
                    arr.push_back(std::move(v));
                    skipWs();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']'");
                }
            }
            out = Json(std::move(arr));
            ok = true;
        } else if (c == '"') {
            std::string str;
            if (!parseString(str)) return false;
            out = Json(std::move(str));
            ok = true;
        } else if (c == 't') {
            if (!match("true")) return fail("invalid literal");
            out = Json(true);
            ok = true;
        } else if (c == 'f') {
            if (!match("false")) return fail("invalid literal");
            out = Json(false);
            ok = true;
        } else if (c == 'n') {
            if (!match("null")) return fail("invalid literal");
            out = Json();
            ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            ok = parseNumber(out);
        } else {
            return fail("unexpected character");
        }
        --depth;
        return ok;
    }
};

Json Json::parse(const std::string& text, std::string* error) {
    Parser p(text);
    // Skip UTF-8 BOM if present.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        p.i = 3;
    }
    Json out;
    if (!p.parseValue(out)) {
        if (error) *error = p.err;
        return Json();
    }
    p.skipWs();
    if (p.i != text.size()) {
        if (error) *error = "trailing characters after JSON value";
        return Json();
    }
    if (error) error->clear();
    return out;
}

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

void Json::escapeString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c); // UTF-8 passthrough
                }
        }
    }
    out.push_back('"');
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(d) * static_cast<std::size_t>(indent), ' ');
    };

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (std::isfinite(num_) && std::floor(num_) == num_ && std::fabs(num_) < 1e18) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
            } else if (std::isfinite(num_)) {
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
            } else {
                std::snprintf(buf, sizeof(buf), "null");
            }
            out += buf;
            break;
        }
        case Type::String: escapeString(str_, out); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (std::size_t k = 0; k < arr_.size(); ++k) {
                if (k) out.push_back(',');
                newline(depth + 1);
                arr_[k].dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (std::size_t k = 0; k < obj_.size(); ++k) {
                if (k) out.push_back(',');
                newline(depth + 1);
                escapeString(obj_[k].first, out);
                out += indent < 0 ? ":" : ": ";
                obj_[k].second.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

} // namespace translator
