#include "translator/TextUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>

namespace translator {
namespace text {

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------

std::vector<std::string> utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) len = s.size() - i;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

namespace {

std::uint32_t codepoint(const std::string& ch) {
    if (ch.empty()) return 0;
    const unsigned char c0 = static_cast<unsigned char>(ch[0]);
    if (c0 < 0x80) return c0;
    if (ch.size() == 2) return ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(ch[1]) & 0x3F);
    if (ch.size() == 3)
        return ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(ch[1]) & 0x3F) << 6) | (static_cast<unsigned char>(ch[2]) & 0x3F);
    if (ch.size() == 4)
        return ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(ch[1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(ch[2]) & 0x3F) << 6) | (static_cast<unsigned char>(ch[3]) & 0x3F);
    return 0;
}

bool isCjkPunct(const std::string& ch) {
    return ch == "\xE3\x80\x82" /* 。 */ || ch == "\xEF\xBC\x8C" /* ， */ || ch == "\xEF\xBC\x81" /* ！ */ ||
           ch == "\xEF\xBC\x9F" /* ？ */ || ch == "\xE3\x80\x81" /* 、 */ || ch == "\xEF\xBC\x9A" /* ： */ ||
           ch == "\xEF\xBC\x9B" /* ； */;
}

bool isAsciiPunct(const std::string& ch) {
    return ch == "." || ch == "," || ch == "!" || ch == "?" || ch == ":" || ch == ";";
}

bool isCjkLang(const std::string& lang) { return lang == "ja" || lang == "zh"; }

std::string toLowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Strip trailing punctuation/spaces for hallucination comparison.
std::string normalizeForMatch(const std::string& s) {
    std::string out;
    for (const auto& ch : utf8Chars(toLowerAscii(s))) {
        if (isAsciiPunct(ch) || isCjkPunct(ch) || ch == " " || ch == "\"" || ch == "'" || ch == "\xE2\x80\x9C" || ch == "\xE2\x80\x9D") continue;
        out += ch;
    }
    return out;
}

const std::vector<std::string>& hallucinations(const std::string& lang) {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"ko", {"시청해주셔서 감사합니다", "시청해 주셔서 감사합니다", "구독과 좋아요 부탁드립니다", "구독 좋아요 부탁드립니다",
                "다음 영상에서 만나요", "자막 제공", "감사합니다 감사합니다", "MBC 뉴스", "KBS 뉴스", "SBS 뉴스",
                "이덕영입니다", "한글자막 by", "자막은 설정에서", "끝까지 시청해주셔서 감사합니다"}},
        {"en", {"thank you for watching", "thanks for watching", "please subscribe", "like and subscribe",
                "subtitles by", "subtitles by the amara.org community", "thank you", "you", "bye", "the end",
                "copyright", "transcribed by", "see you in the next video"}},
        {"ja", {"ご視聴ありがとうございました", "ご視聴ありがとうございます", "チャンネル登録お願いします",
                "字幕", "おやすみなさい", "ありがとうございました"}},
        {"zh", {"谢谢观看", "感谢观看", "请订阅", "字幕由", "字幕志愿者", "中文字幕", "谢谢大家", "明镜与点点栏目"}},
        {"es", {"gracias por ver", "suscríbete", "subtítulos realizados por", "subtítulos por la comunidad de amara.org",
                "gracias", "hasta la próxima"}},
    };
    static const std::vector<std::string> empty;
    const auto it = table.find(lang);
    return it == table.end() ? empty : it->second;
}

} // namespace

bool isCjkChar(const std::string& ch) {
    const std::uint32_t cp = codepoint(ch);
    return (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana, Katakana
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compat
           (cp >= 0xFF00 && cp <= 0xFFEF);     // full-width forms
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string collapseSpaces(const std::string& s) {
    std::string out;
    bool prevSpace = false;
    for (const char c : s) {
        const bool sp = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (sp) {
            if (!prevSpace) out.push_back(' ');
        } else {
            out.push_back(c);
        }
        prevSpace = sp;
    }
    return trim(out);
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

std::string defaultPromptFor(const std::string& lang) {
    if (lang == "ko") return "안녕하세요. 오늘 회의는 오후 3시에 시작합니다. 네, 알겠습니다. 감사합니다.";
    if (lang == "en") return "Hello. Today's meeting starts at 3 p.m. Yes, I understand. Thank you.";
    if (lang == "ja") return "こんにちは。今日の会議は午後3時に始まります。はい、わかりました。ありがとうございます。";
    if (lang == "zh") return "你好。今天的会议下午三点开始。好的，我明白了。谢谢。";
    if (lang == "es") return "Hola. La reunión de hoy empieza a las tres de la tarde. Sí, entendido. Gracias.";
    return {};
}

std::string buildSttPrompt(const std::string& userPrompt, const std::string& lang,
                           const std::string& previousTranscript, std::size_t maxContextChars) {
    std::string prompt = userPrompt.empty() ? defaultPromptFor(lang) : userPrompt;
    const std::string prev = trim(previousTranscript);
    if (!prev.empty() && maxContextChars > 0) {
        const auto chars = utf8Chars(prev);
        std::string tail;
        const std::size_t start = chars.size() > maxContextChars ? chars.size() - maxContextChars : 0;
        for (std::size_t i = start; i < chars.size(); ++i) tail += chars[i];
        // Don't start mid-word: drop the first partial token when we cut.
        if (start > 0) {
            const auto sp = tail.find(' ');
            if (sp != std::string::npos && sp + 1 < tail.size()) tail = tail.substr(sp + 1);
        }
        prompt = prompt.empty() ? tail : prompt + " " + tail;
    }
    return prompt;
}

// ---------------------------------------------------------------------------
// Transcript cleaning
// ---------------------------------------------------------------------------

bool isHallucination(const std::string& s, const std::string& lang) {
    const std::string norm = normalizeForMatch(s);
    if (norm.empty()) return true;
    auto matches = [&](const std::vector<std::string>& list) {
        for (const auto& h : list) {
            const std::string hn = normalizeForMatch(h);
            if (hn.empty()) continue;
            if (norm == hn) return true;
            // The stock phrase repeated ("감사합니다 감사합니다 감사합니다") is still a hallucination.
            if (norm.size() > hn.size() && norm.size() % hn.size() == 0) {
                bool allSame = true;
                for (std::size_t off = 0; off < norm.size() && allSame; off += hn.size())
                    allSame = norm.compare(off, hn.size(), hn) == 0;
                if (allSame) return true;
            }
        }
        return false;
    };
    if (matches(hallucinations(lang))) return true;
    if (lang.empty() || lang == "auto") {
        for (const char* l : {"ko", "en", "ja", "zh", "es"})
            if (matches(hallucinations(l))) return true;
    }
    return false;
}

std::string cleanTranscript(const std::string& input, const std::string& lang) {
    // 1. Remove bracketed markers: [음악], (박수), 【字幕】, ♪ ... ♪
    std::string s;
    int depth = 0;
    for (const auto& ch : utf8Chars(input)) {
        if (ch == "[" || ch == "(" || ch == "\xE3\x80\x90" /* 【 */ || ch == "\xEF\xBC\x88" /* （ */) { ++depth; continue; }
        if (ch == "]" || ch == ")" || ch == "\xE3\x80\x91" /* 】 */ || ch == "\xEF\xBC\x89" /* ） */) { if (depth > 0) --depth; continue; }
        if (depth > 0) continue;
        if (ch == "\xE2\x99\xAA" /* ♪ */ || ch == "\xE2\x99\xAB" /* ♫ */) continue;
        s += ch;
    }
    s = collapseSpaces(s);
    if (s.empty()) return {};

    // 2. Runaway repetition: the same word (or CJK bigram) more than 4 times in a row.
    {
        std::vector<std::string> words;
        std::string cur;
        for (const char c : s) {
            if (c == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) words.push_back(cur);
        std::vector<std::string> kept;
        int run = 0;
        for (const auto& w : words) {
            if (!kept.empty() && kept.back() == w) {
                if (++run >= 3) continue; // keep at most 3 consecutive copies
            } else {
                run = 0;
            }
            kept.push_back(w);
        }
        s.clear();
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (i) s.push_back(' ');
            s += kept[i];
        }
        // CJK strings have no spaces: collapse a 2–6 character unit repeated ≥ 4 times in a
        // row down to two copies (e.g. "감사합니다감사합니다감사합니다감사합니다…").
        for (std::size_t unit = 2; unit <= 6; ++unit) {
            const auto chars = utf8Chars(s);
            if (chars.size() < unit * 4) break;
            std::string rebuilt;
            std::size_t i = 0;
            while (i < chars.size()) {
                std::size_t reps = 1;
                while (i + (reps + 1) * unit <= chars.size() &&
                       std::equal(chars.begin() + static_cast<std::ptrdiff_t>(i),
                                  chars.begin() + static_cast<std::ptrdiff_t>(i + unit),
                                  chars.begin() + static_cast<std::ptrdiff_t>(i + reps * unit)))
                    ++reps;
                if (reps >= 4) {
                    for (std::size_t r = 0; r < 2; ++r)
                        for (std::size_t k = 0; k < unit; ++k) rebuilt += chars[i + k];
                    i += reps * unit;
                } else {
                    rebuilt += chars[i];
                    ++i;
                }
            }
            s = rebuilt;
        }
    }

    // 3. Drop known hallucinated stock phrases (whole output, or sentence by sentence).
    if (isHallucination(s, lang)) return {};
    return trim(s);
}

// ---------------------------------------------------------------------------
// Translation post-processing
// ---------------------------------------------------------------------------

std::string postProcessTranslation(const std::string& input, const std::string& lang) {
    std::string s = collapseSpaces(input);
    if (s.empty()) return s;
    const auto chars = utf8Chars(s);
    std::string out;
    const bool cjkTarget = isCjkLang(lang);

    for (std::size_t i = 0; i < chars.size(); ++i) {
        const std::string& ch = chars[i];
        if (ch == " ") {
            const std::string& next = i + 1 < chars.size() ? chars[i + 1] : std::string();
            const std::string& prev = i > 0 ? chars[i - 1] : std::string();
            // No space before punctuation (any language) …
            if (isAsciiPunct(next) || isCjkPunct(next) || next == ")" || next == "\xEF\xBC\x89") continue;
            // … after an opening bracket …
            if (prev == "(" || prev == "\xEF\xBC\x88") continue;
            // … and, for ja/zh, between CJK characters or around any punctuation (Latin words
            // embedded in a CJK sentence keep the space between them).
            if (cjkTarget) {
                const bool prevCjk = isCjkChar(prev) || isCjkPunct(prev) || isAsciiPunct(prev);
                const bool nextCjk = isCjkChar(next) || isCjkPunct(next) || isAsciiPunct(next);
                if (prevCjk && nextCjk) continue;
                if (isCjkPunct(prev) || isAsciiPunct(prev)) continue;
            }
        }
        out += ch;
    }

    if (lang == "ko") {
        // Korean keeps spaces between words but not before punctuation. Some models glue the
        // next sentence to the period ("시작됩니다.이 제품은") — reinsert the space.
        std::string spaced;
        const auto kc = utf8Chars(out);
        for (std::size_t i = 0; i < kc.size(); ++i) {
            spaced += kc[i];
            if ((kc[i] == "." || kc[i] == "!" || kc[i] == "?") && i + 1 < kc.size()) {
                const std::string& nx = kc[i + 1];
                const bool digit = nx.size() == 1 && std::isdigit(static_cast<unsigned char>(nx[0]));
                if (nx != " " && !isAsciiPunct(nx) && !isCjkPunct(nx) && nx != "\"" && nx != "'" && nx != ")" && !(kc[i] == "." && digit))
                    spaced += " ";
            }
        }
        return trim(spaced);
    }
    if (cjkTarget) {
        // Prefer full-width sentence punctuation in Japanese / Chinese output.
        std::string fw;
        for (const auto& ch : utf8Chars(out)) {
            if (ch == ".") fw += "\xE3\x80\x82";
            else if (ch == ",") fw += lang == "ja" ? "\xE3\x80\x81" : "\xEF\xBC\x8C";
            else if (ch == "?") fw += "\xEF\xBC\x9F";
            else if (ch == "!") fw += "\xEF\xBC\x81";
            else fw += ch;
        }
        return trim(fw);
    }

    // Latin-script targets: capitalize the first letter of each sentence, ensure a space
    // after sentence punctuation.
    std::string cap;
    bool sentenceStart = true;
    const auto oc = utf8Chars(out);
    for (std::size_t i = 0; i < oc.size(); ++i) {
        std::string ch = oc[i];
        if (sentenceStart && ch.size() == 1 && std::isalpha(static_cast<unsigned char>(ch[0]))) {
            ch[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ch[0])));
            sentenceStart = false;
        } else if (ch == "." || ch == "!" || ch == "?") {
            // Sentence boundary only when followed by a space / quote / end. A period glued to
            // the next character is an abbreviation or a number ("p.m.", "e.g.", "3.14", "a.b.c").
            const bool atEnd = i + 1 >= oc.size();
            const std::string& nx = atEnd ? std::string() : oc[i + 1];
            bool boundary = atEnd || nx == " " || nx == "\"" || nx == "'" || nx == ")" || isAsciiPunct(nx);
            if (boundary && ch == ".") {
                // "e.g. tomorrow", "Mr. Smith": the word before the period is an abbreviation.
                static const char* const kAbbrev[] = {"e.g", "i.e", "etc", "mr", "mrs", "ms", "dr", "prof", "vs", "st", "no", "sr", "jr", "approx", "dept", "inc", "ltd"};
                const auto sp = cap.find_last_of(' ');
                const std::string word = toLowerAscii(sp == std::string::npos ? cap : cap.substr(sp + 1));
                for (const char* a : kAbbrev)
                    if (word == a) { boundary = false; break; }
            }
            cap += ch;
            if (boundary) sentenceStart = true;
            continue;
        } else if (ch != " " && ch != "\"" && ch != "'" && ch != "\xC2\xBF" && ch != "\xC2\xA1") {
            sentenceStart = false;
        }
        cap += ch;
    }
    return trim(cap);
}

} // namespace text
} // namespace translator
