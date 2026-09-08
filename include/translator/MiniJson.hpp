// Minimal JSON value + parser + serializer (RFC 8259 subset sufficient for manifests).
// Header-only-free single implementation; no external dependency required on mobile.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace translator {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>; // insertion-ordered

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    // One overload per distinct integer type so int64_t/uint64_t/size_t/long long all resolve
    // unambiguously on every platform (int64_t is `long` on Linux, `long long` on Windows).
    Json(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(unsigned v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(unsigned long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(unsigned long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Json(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    // Parsing / serialization -------------------------------------------------
    // On failure returns a Null Json and, if provided, fills *error with a message.
    static Json parse(const std::string& text, std::string* error = nullptr);
    // indent < 0 → compact; otherwise pretty-printed with the given indent width.
    std::string dump(int indent = -1) const;

    // Type queries ------------------------------------------------------------
    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Value accessors (with defaults for lenient reads) ------------------------
    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0.0) const { return isNumber() ? num_ : def; }
    std::int64_t asInt64(std::int64_t def = 0) const { return isNumber() ? static_cast<std::int64_t>(num_) : def; }
    int asInt(int def = 0) const { return isNumber() ? static_cast<int>(num_) : def; }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return isString() ? str_ : def; }

    const Array& asArray() const;
    const Object& asObject() const;
    Array& asArray();
    Object& asObject();

    // Object helpers ----------------------------------------------------------
    bool contains(const std::string& key) const;
    const Json& operator[](const std::string& key) const; // Null sentinel if missing
    Json& operator[](const std::string& key);             // inserts Null if missing
    const Json& at(std::size_t i) const;                  // Null sentinel if out of range
    std::size_t size() const;

    // Convenience typed getters with defaults.
    std::string getString(const std::string& key, const std::string& def = "") const { return (*this)[key].asString(def); }
    std::int64_t getInt64(const std::string& key, std::int64_t def = 0) const { return (*this)[key].asInt64(def); }
    bool getBool(const std::string& key, bool def = false) const { return (*this)[key].asBool(def); }

    void push_back(Json v);
    void set(const std::string& key, Json v) { (*this)[key] = std::move(v); }

private:
    struct Parser;
    void dumpTo(std::string& out, int indent, int depth) const;
    static void escapeString(const std::string& s, std::string& out);

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

} // namespace translator
