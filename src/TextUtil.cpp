#include "translator/TextUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
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

// Lower-cases the Cyrillic capitals (U+0410–U+042F → U+0430–U+044F, Ё → ё) so a Russian stock
// phrase matches however whisper capitalised it; other scripts pass through untouched.
std::string toLowerCyrillic(const std::string& ch) {
    if (ch.size() != 2) return ch;
    const unsigned char b0 = static_cast<unsigned char>(ch[0]), b1 = static_cast<unsigned char>(ch[1]);
    if (b0 == 0xD0 && b1 >= 0x90 && b1 <= 0x9F) return std::string{static_cast<char>(0xD0), static_cast<char>(b1 + 0x20)};  // А–П
    if (b0 == 0xD0 && b1 >= 0xA0 && b1 <= 0xAF) return std::string{static_cast<char>(0xD1), static_cast<char>(b1 - 0x20)};  // Р–Я
    if (b0 == 0xD0 && b1 == 0x81) return std::string{static_cast<char>(0xD1), static_cast<char>(0x91)};                     // Ё
    return ch;
}

// Drop punctuation and spaces so two spellings of the same sentence compare equal: used for the
// hallucination list and for the fixed-phrase table, where whisper's commas, hyphens and curly
// apostrophes must not decide whether a phrase is found.
std::string normalizeForMatch(const std::string& s) {
    std::string out;
    for (const auto& ch : utf8Chars(toLowerAscii(s))) {
        if (isAsciiPunct(ch) || isCjkPunct(ch) || ch == " " || ch == "\"" || ch == "'" || ch == "-" ||
            ch == "\xE2\x80\x9C" || ch == "\xE2\x80\x9D" || ch == "\xE2\x80\x98" || ch == "\xE2\x80\x99" ||
            ch == "\xE2\x80\x93" || ch == "\xE2\x80\x94" || ch == "\xE2\x80\xA6") continue;
        out += toLowerCyrillic(ch);
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
        {"vi", {"cảm ơn các bạn đã theo dõi", "cảm ơn đã xem", "hãy đăng ký kênh", "hẹn gặp lại", "phụ đề bởi", "cảm ơn"}},
        {"th", {"ขอบคุณที่รับชม", "ขอบคุณครับ", "ขอบคุณค่ะ", "กดติดตาม", "แล้วพบกันใหม่", "คำบรรยายโดย"}},
        {"id", {"terima kasih telah menonton", "terima kasih sudah menonton", "jangan lupa subscribe",
                "sampai jumpa di video berikutnya", "subtitle oleh", "terima kasih"}},
        {"fr", {"merci d'avoir regardé", "merci de votre attention", "abonnez-vous", "sous-titres réalisés par",
                "sous-titrage société radio-canada", "sous-titres par la communauté d'amara.org", "merci", "à la prochaine"}},
        {"ru", {"спасибо за просмотр", "подписывайтесь на канал", "субтитры сделал", "субтитры создавал",
                "редактор субтитров", "продолжение следует", "спасибо", "до новых встреч"}},
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
    if (lang == "vi") return "Xin chào. Cuộc họp hôm nay bắt đầu lúc 3 giờ chiều. Vâng, tôi hiểu rồi. Cảm ơn.";
    if (lang == "th") return "สวัสดีครับ การประชุมวันนี้เริ่มตอนบ่ายสามโมง ครับ เข้าใจแล้ว ขอบคุณครับ";
    if (lang == "id") return "Halo. Rapat hari ini dimulai pukul 3 sore. Ya, saya mengerti. Terima kasih.";
    if (lang == "fr") return "Bonjour. La réunion d'aujourd'hui commence à 15 heures. Oui, je comprends. Merci.";
    if (lang == "ru") return "Здравствуйте. Сегодняшняя встреча начинается в три часа дня. Да, я понимаю. Спасибо.";
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

namespace {
// The en-ko Marian model now and then appends its English input to the Korean ("... 걸립니까? How
// long is the ride to the airport in a cab?"). Behind text in a non-Latin target script, a run of
// four or more Latin-alphabet words at the very end is that copy, never the translation.
std::string dropAppendedSource(const std::string& s, const std::string& lang) {
    if (lang != "ko" && lang != "ja" && lang != "zh" && lang != "th" && lang != "ru") return s;
    const auto chars = utf8Chars(s);
    auto latinWord = [](const std::string& ch) {
        if (ch.size() == 1) return true;                       // ASCII letters, digits, punctuation
        return ch == "’" || ch == "‘";  // curly apostrophes
    };
    // Walk words from the end while they are made of Latin/ASCII characters only.
    std::size_t i = chars.size();
    while (i > 0 && chars[i - 1] == " ") --i;
    std::size_t cut = i;
    int words = 0;
    while (i > 0) {
        std::size_t j = i;
        bool letters = false, ok = true;
        while (j > 0 && chars[j - 1] != " ") {
            --j;
            if (!latinWord(chars[j])) { ok = false; break; }
            const char c = chars[j][0];
            if (chars[j].size() == 1 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) letters = true;
        }
        if (!ok || !letters) break;
        cut = j;
        ++words;
        i = j;
        while (i > 0 && chars[i - 1] == " ") --i;
    }
    if (words < 4 || cut == 0) return s;
    bool nativeBefore = false;
    for (std::size_t k = 0; k < cut && !nativeBefore; ++k) nativeBefore = chars[k].size() > 1 && !latinWord(chars[k]);
    if (!nativeBefore) return s;
    std::string head;
    for (std::size_t k = 0; k < cut; ++k) head += chars[k];
    return trim(head);
}
} // namespace

std::string postProcessTranslation(const std::string& input, const std::string& lang) {
    std::string s = dropAppendedSource(collapseSpaces(input), lang);
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

    if (lang == "th") {
        // Thai has no sentence-final punctuation; a space *is* the sentence separator.
        return trim(out);
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


namespace {

// Short set phrases the OPUS-MT models get wrong, or answer in a register an interpreter between
// strangers cannot use. A sentence-level model with no context is at its weakest here: measured on
// the desktop (translator_cli translate --backend marian, 2026-09-22), ko-en answered "안녕하세요."
// with "Good evening.", "목이 말라요." with "Not the throat.", "싫어요." with "Okay." (the opposite),
// "예약했습니다." with "About Us"; en-ko answered "No." with "안 돼" (= you may not), "Check, please."
// with "확인해 주세요." (= please verify), "This way, please." with "이쪽으로, please." and a long list
// of everyday phrases in 반말, which is rude between people who have just met.
//
// An entry only fires on a whole bare sentence, so a greeting inside a longer one still goes to
// Marian ("안녕하세요, 저는 김입니다." → "Hi, I'm Kim."). Sentences whose meaning turns over when
// they are asked rather than stated ("네?" is "Pardon?", not "Yes.") are marked statementOnly and
// are left to the model when the sentence ends in a question mark.
struct FixedPhrase {
    const char* from;    // written as it is spoken; matched through normalizeForMatch()
    const char* to;      // the answer, punctuation and all
    bool statementOnly;  // skip when the sentence is a question
};

const FixedPhrase kFixedKoEn[] = {
    // Greetings and farewells. "안녕하세요." is the one the app hit on every conversation.
    {"안녕하세요", "Hello.", false},
    {"안녕하십니까", "Hello.", false},
    {"안녕", "Hi.", false},
    {"반갑습니다", "Nice to meet you.", false},
    {"반가워요", "Nice to meet you.", false},
    {"처음 뵙겠습니다", "Nice to meet you.", false},           // "See you soon."
    {"오랜만입니다", "It's been a long time.", false},          // "Long time."
    {"오랜만이에요", "It's been a long time.", false},
    {"오랜만이네요", "It's been a long time.", false},
    {"어서 오십시오", "Welcome.", false},                       // "Welcome back."
    {"그럼 이만 가보겠습니다", "I'll be going now.", false},     // "So let's go."
    {"연락드리겠습니다", "I'll be in touch.", false},            // "About Us"
    // Yes, no and the short answers. Marian's "Yeah." / "Nope." are too casual for an interpreter,
    // and "싫어요." came out as "Okay." — the opposite of what was said.
    {"네", "Yes.", true},
    {"예", "Yes.", true},
    {"아니요", "No.", true},
    {"아니오", "No.", true},
    {"아닙니다", "No.", true},
    {"맞아요", "That's right.", true},
    {"맞습니다", "That's right.", true},
    {"알겠습니다", "I understand.", true},
    {"네, 알겠습니다", "Yes, I understand.", true},             // "Yes, sir."
    {"싫어요", "I don't want to.", true},                       // "Okay."
    {"몰라요", "I don't know.", true},
    // Thanks and apologies: 합쇼체 in, the full form out.
    {"감사합니다", "Thank you.", false},
    {"죄송합니다", "I'm sorry.", false},
    {"죄송해요", "I'm sorry.", false},
    {"미안합니다", "I'm sorry.", false},
    {"미안해요", "I'm sorry.", false},
    {"수고하셨습니다", "Thank you for your hard work.", false},
    {"수고 많으셨습니다", "Thank you for your hard work.", false},
    {"잘 먹겠습니다", "Thank you for the meal.", false},         // "I'll eat."
    {"잘 먹었습니다", "Thank you for the meal.", false},         // "I ate well."
    // Paying and ordering: 계산 is the bill here, not arithmetic.
    {"계산해 주세요", "Check, please.", false},                  // "Please calculate."
    {"계산 좀 해주세요", "Check, please.", false},               // "Give me the calculation."
    {"계산서 주세요", "The bill, please.", false},               // "Give me the account."
    {"카드로 결제할게요", "I'll pay by card.", false},            // "I'll finish the card."
    {"카드로 계산할게요", "I'll pay by card.", false},            // "Count to card."
    {"예약했어요", "I have a reservation.", false},              // "Reservationd."
    {"예약했습니다", "I have a reservation.", false},            // "About Us"
    // Everyday sentences that came back wrong.
    {"목이 말라요", "I'm thirsty.", false},                      // "Not the throat."
    {"한국어를 조금 할 수 있어요", "I can speak a little Korean.", false}, // "I can do a little Korean."
    {"누구세요", "Who is it?", false},                           // "Hello?"
    {"어떡하죠", "What should I do?", false},                    // "What?"
    {"어떡하지", "What should I do?", false},
    {"잠시만요", "Just a moment.", false},                       // "Wait."
    {"잠깐만요", "Just a moment.", false},                       // "Wait."
    {"도와주세요", "Please help me.", false},                    // "Help."
};

const FixedPhrase kFixedEnKo[] = {
    // Greetings and farewells. Marian's bare "Hello." was the phone greeting "여보세요?".
    {"Hello.", "안녕하세요.", false},
    {"Hi.", "안녕하세요.", false},
    {"Hey.", "안녕하세요.", false},
    {"Good morning.", "좋은 아침입니다.", false},
    {"Good night.", "안녕히 주무세요.", false},
    {"Goodbye.", "안녕히 가세요.", false},
    {"Bye.", "안녕히 가세요.", false},
    {"Bye bye.", "안녕히 가세요.", false},
    {"See you.", "나중에 뵙겠습니다.", false},
    {"See you later.", "나중에 뵙겠습니다.", false},
    {"Take care.", "조심히 가세요.", false},
    {"It's been a while.", "오랜만입니다.", false},
    {"Welcome.", "어서 오세요.", false},
    {"Congratulations.", "축하합니다.", false},
    {"Happy birthday.", "생일 축하합니다.", false},
    {"Cheers.", "건배.", false},
    {"Enjoy your meal.", "맛있게 드세요.", false},
    // Yes, no and the short answers: "No." came back as "안 돼" (= you may not).
    {"Yes.", "네.", true},
    {"No.", "아니요.", true},
    {"Okay.", "알겠습니다.", true},
    {"OK.", "알겠습니다.", true},
    {"All right.", "알겠습니다.", true},
    {"Got it.", "알겠습니다.", true},
    {"That's right.", "맞습니다.", true},
    {"I see.", "그렇군요.", true},
    {"I don't know.", "잘 모르겠어요.", true},                   // "나도 몰라." (= I don't know either)
    {"Never mind.", "괜찮습니다.", true},
    {"Well done.", "잘하셨습니다.", true},
    {"Of course.", "물론이죠.", false},
    // Thanks and apologies.
    {"Sorry.", "죄송합니다.", false},
    {"I'm sorry.", "죄송합니다.", false},
    {"I am sorry.", "죄송합니다.", false},
    {"Excuse me.", "실례합니다.", false},
    // Asking for help and for a repeat: all of these came back in 반말.
    {"Help me.", "도와주세요.", false},
    {"Please help me.", "도와주세요.", false},
    {"I need help.", "도움이 필요해요.", false},
    {"Can you help me?", "도와주시겠어요?", false},
    {"I don't understand.", "이해하지 못했어요.", false},
    {"Can you say that again?", "다시 한번 말씀해 주시겠어요?", false},
    {"Could you say that again?", "다시 한번 말씀해 주시겠어요?", false},
    {"Can you speak more slowly?", "조금 더 천천히 말씀해 주시겠어요?", false},
    {"Could you speak more slowly?", "조금 더 천천히 말씀해 주시겠어요?", false},
    {"Do you speak English?", "영어 하실 줄 아세요?", false},
    {"Do you speak Korean?", "한국어 하실 줄 아세요?", false},
    {"What do you mean?", "무슨 뜻이에요?", false},              // "무슨 소리야?"
    {"What's your name?", "이름이 어떻게 되세요?", false},
    {"How are you?", "어떻게 지내세요?", false},
    {"How's it going?", "어떻게 지내세요?", false},
    // Paying, ordering and getting about.
    {"How much?", "얼마예요?", false},                           // "얼마나?"
    {"How much is this?", "이거 얼마예요?", false},
    {"How much is it?", "얼마예요?", false},
    {"Check, please.", "계산해 주세요.", false},                  // "확인해 주세요." (= please verify)
    {"Could I get the check?", "계산해 주세요.", false},          // "수표 좀 주실래요?" (= a bank cheque)
    {"The bill, please.", "계산서 주세요.", false},               // "빌, 제발"
    {"I'll pay by card.", "카드로 결제할게요.", false},
    {"I'll pay in cash.", "현금으로 낼게요.", false},
    {"I'm thirsty.", "목이 말라요.", false},
    {"Just a moment.", "잠시만요.", false},
    {"Hold on.", "잠시만요.", false},
    {"Let's go.", "갑시다.", false},
    {"Follow me.", "따라오세요.", false},
    {"This way, please.", "이쪽으로 오세요.", false},             // "이쪽으로, please."
    {"Go straight.", "직진하세요.", false},
    {"Turn left.", "왼쪽으로 가세요.", false},
    {"Turn right.", "오른쪽으로 가세요.", false},                 // "오른쪽으로 돌려." (= rotate it)
    {"Have a seat.", "앉으세요.", false},
    {"Is this seat taken?", "이 자리 주인 있나요?", false},       // "이 자리는?"
};

// Keyed by normalizeForMatch(from), built once. A duplicate key would silently shadow its
// neighbour, so the table is checked for one in the tests.
const std::map<std::string, const FixedPhrase*>* fixedTable(const std::string& src, const std::string& tgt) {
    static const std::map<std::string, std::map<std::string, const FixedPhrase*>> index = [] {
        std::map<std::string, std::map<std::string, const FixedPhrase*>> m;
        for (const FixedPhrase& e : kFixedKoEn) m["ko-en"][normalizeForMatch(e.from)] = &e;
        for (const FixedPhrase& e : kFixedEnKo) m["en-ko"][normalizeForMatch(e.from)] = &e;
        return m;
    }();
    const auto it = index.find(src + "-" + tgt);
    return it == index.end() ? nullptr : &it->second;
}

} // namespace

std::string fixedTranslation(const std::string& sentence, const std::string& src, const std::string& tgt) {
    const auto* table = fixedTable(src, tgt);
    if (!table) return "";
    const std::string bare = trim(sentence);
    if (bare.empty()) return "";
    const std::vector<std::string> chars = utf8Chars(bare);
    const bool question = !chars.empty() && (chars.back() == "?" || chars.back() == "\xEF\xBC\x9F" /* ？ */);
    const std::string key = normalizeForMatch(bare);
    if (key.empty()) return "";
    const auto it = table->find(key);
    if (it == table->end()) return "";
    if (question && it->second->statementOnly) return "";
    return it->second->to;
}

std::size_t fixedTranslationCount(const std::string& src, const std::string& tgt) {
    const auto* table = fixedTable(src, tgt);
    return table ? table->size() : 0;
}

} // namespace text
} // namespace translator
