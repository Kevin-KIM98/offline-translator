#include "translator/LlmEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#if TRANSLATOR_HAS_LLAMA
#include "llama.h"
#endif

namespace translator {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip decorations small models sometimes add despite the instruction.
std::string cleanOutput(std::string s) {
    s = trim(s);
    // "Translation: ..." / "번역: ..." prefixes
    static const char* const prefixes[] = {"Translation:", "translation:", "Translated text:", "Output:", "번역:", "翻译:", "翻訳:", "Traducción:", "Bản dịch:", "คำแปล:"};
    for (const char* p : prefixes) {
        const std::size_t n = std::strlen(p);
        if (s.compare(0, n, p) == 0) {
            s = trim(s.substr(n));
            break;
        }
    }
    // Surrounding quotes
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = trim(s.substr(1, s.size() - 2));
    // Keep only the first paragraph if the model started explaining.
    const auto nl = s.find("\n\n");
    if (nl != std::string::npos) s = trim(s.substr(0, nl));
    if (const auto n1 = s.find('\n'); n1 != std::string::npos) s = trim(s.substr(0, n1));
    // Cut trailing translator's notes: "(Note: ...)", "（注：...）", "(참고: ...)".
    static const char* const noteMarkers[] = {"(Note", "(note", "Note:", "\xEF\xBC\x88\xE6\xB3\xA8", "(\xE6\xB3\xA8", "\xE6\xB3\xA8\xEF\xBC\x9A",
                                              "(\xEC\xB0\xB8\xEA\xB3\xA0", "\xEC\xB0\xB8\xEA\xB3\xA0:", "(\xE1\xB8\xB1\xE1\xBB\x9Bt", "\xEC\x9D\xB4\xEB\xB2\x88 \xEB\xB2\x88\xEC\x97\xAD"};
    for (const char* m : noteMarkers) {
        const auto pos = s.find(m);
        if (pos != std::string::npos && pos > 0) s = trim(s.substr(0, pos));
    }
    // Collapse a word/unit repeated ≥ 3 times at the end ("... ครับ ครับ ครับ").
    for (int pass = 0; pass < 3; ++pass) {
        const auto sp = s.find_last_of(' ');
        if (sp == std::string::npos) break;
        const std::string last = s.substr(sp + 1);
        if (last.empty()) break;
        std::size_t reps = 0;
        std::string cur = s;
        while (cur.size() >= last.size() && cur.compare(cur.size() - last.size(), last.size(), last) == 0) {
            cur = trim(cur.substr(0, cur.size() - last.size()));
            ++reps;
            if (cur.empty()) break;
        }
        if (reps >= 3) s = trim(cur + " " + last);
        else break;
    }
    return s;
}

} // namespace

std::string LlmEngine::languageName(const std::string& code) {
    if (code == "ko") return "Korean";
    if (code == "en") return "English";
    if (code == "es") return "Spanish";
    if (code == "vi") return "Vietnamese";
    if (code == "th") return "Thai";
    if (code == "ja") return "Japanese";
    if (code == "zh") return "Chinese (Simplified)";
    if (code == "fr") return "French";
    if (code == "de") return "German";
    if (code == "pt") return "Portuguese";
    if (code == "id") return "Indonesian";
    if (code == "ru") return "Russian";
    if (code == "ar") return "Arabic";
    return code;
}

std::string LlmEngine::exampleSentence(const std::string& code) {
    if (code == "ko") return "실례합니다, 가장 가까운 역이 어디인가요?";
    if (code == "en") return "Excuse me, where is the nearest station?";
    if (code == "es") return "Disculpe, ¿dónde está la estación más cercana?";
    if (code == "vi") return "Xin lỗi, ga gần nhất ở đâu ạ?";
    if (code == "th") return "ขอโทษครับ สถานีที่ใกล้ที่สุดอยู่ที่ไหนครับ";
    if (code == "ja") return "すみません、一番近い駅はどこですか？";
    if (code == "zh") return "请问，最近的车站在哪里？";
    return {};
}

std::vector<std::string> LlmEngine::exampleSet(const std::string& code) {
    // A statement with a day, a polite request and a count. Varied, so the model picks up the task
    // rather than one sentence shape, and far from travel questions, so nothing leaks into them.
    if (code == "ko") return {"회의가 금요일로 미뤄졌어요.", "음악 소리를 조금만 줄여 주시겠어요?", "어제 책을 두 권 샀어요."};
    if (code == "en") return {"The meeting has been moved to Friday.", "Could you turn the music down a little?", "I bought two books yesterday."};
    if (code == "es") return {"La reunión se ha aplazado al viernes.", "¿Podría bajar un poco la música?", "Ayer compré dos libros."};
    if (code == "vi") return {"Cuộc họp đã được dời sang thứ Sáu.", "Bạn có thể vặn nhỏ nhạc một chút được không?", "Hôm qua tôi đã mua hai cuốn sách."};
    if (code == "th") return {"การประชุมถูกเลื่อนไปเป็นวันศุกร์ครับ", "ช่วยเบาเสียงเพลงลงหน่อยได้ไหมครับ", "เมื่อวานผมซื้อหนังสือสองเล่มครับ"};
    if (code == "ja") return {"会議は金曜日に延期されました。", "音楽を少し小さくしていただけますか？", "昨日、本を二冊買いました。"};
    if (code == "zh") return {"会议推迟到星期五了。", "可以把音乐调小一点吗？", "我昨天买了两本书。"};
    return {};
}

namespace {

std::uint32_t decodeUtf8(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp;
    std::size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if (c >= 0xF0) { cp = c & 0x07; len = 4; }
    else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
    else { cp = c & 0x1F; len = 2; }
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}

enum class Script { Latin, Hangul, Han, Kana, Thai, Jamo, Other };

Script scriptOf(std::uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) return Script::Latin;
    if (cp >= 0xAC00 && cp <= 0xD7AF) return Script::Hangul;
    // Conjoining / compatibility jamo never appear in normal Korean text (precomposed syllables do).
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xD7B0 && cp <= 0xD7FF)) return Script::Jamo;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF)) return Script::Han;
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) return Script::Kana;
    if (cp >= 0x0E00 && cp <= 0x0E7F) return Script::Thai;
    return Script::Other;
}

bool scriptAllowed(Script s, const std::string& lang) {
    if (s == Script::Jamo) return false;
    if (lang == "ko") return s == Script::Hangul;                 // Hanja is not expected in modern Korean output
    if (lang == "ja") return s == Script::Kana || s == Script::Han;
    if (lang == "zh") return s == Script::Han;
    if (lang == "th") return s == Script::Thai;
    if (lang == "en" || lang == "es" || lang == "vi") return s == Script::Latin;
    return true;
}

const char* scriptName(const std::string& lang) {
    if (lang == "ko") return "Hangul";
    if (lang == "ja") return "Japanese (kana and kanji)";
    if (lang == "zh") return "Simplified Chinese characters";
    if (lang == "th") return "Thai";
    return "the Latin alphabet";
}

} // namespace

double LlmEngine::foreignScriptRatio(const std::string& text, const std::string& lang) {
    std::size_t i = 0, letters = 0, foreign = 0;
    while (i < text.size()) {
        const Script s = scriptOf(decodeUtf8(text, i));
        if (s == Script::Other) continue;
        ++letters;
        if (!scriptAllowed(s, lang)) ++foreign;
    }
    return letters == 0 ? 0.0 : static_cast<double>(foreign) / static_cast<double>(letters);
}

std::string LlmEngine::buildInstruction(const std::string& src, const std::string& tgt, const std::string& systemPrompt) {
    if (!systemPrompt.empty()) return systemPrompt;
    const std::string target = languageName(tgt);
    std::string s = "You are a professional interpreter. ";
    if (src.empty() || src == "auto") s += "Detect the language of the user's message and translate it into " + target + ". ";
    else s += "Translate the user's message from " + languageName(src) + " into " + target + ". ";
    s += "Rules: output ONLY the " + target + " translation, nothing else; no explanations, no notes, no quotes; "
         "write every word in " + target + " using " + scriptName(tgt) + " (never mix in other languages or scripts); "
         "keep the meaning, tone and politeness level; keep numbers, names, times and units unchanged; "
         "if the message is already in " + target + ", output it unchanged.";
    return s;
}

// ---------------------------------------------------------------------------

struct LlmEngine::Impl {
#if TRANSLATOR_HAS_LLAMA
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::string chatTemplate;
    // One sampler chain per target language: a logit-bias stage that forbids tokens written in
    // scripts foreign to the target (Han/Kana in Korean, Hangul in Thai, ...) followed by the
    // greedy / sampling stage. This structurally prevents the language mixing small models show.
    std::map<std::string, llama_sampler*> samplers;
    std::vector<std::vector<Script>> tokenScripts; // per vocab id: scripts of the letters in its piece

    void classifyVocab() {
        const int n = llama_vocab_n_tokens(vocab);
        tokenScripts.assign(static_cast<std::size_t>(n), {});
        char buf[128];
        for (int id = 0; id < n; ++id) {
            const int len = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
            if (len <= 0) continue;
            const std::string piece(buf, static_cast<std::size_t>(len));
            // Partial UTF-8 (byte-level BPE fallbacks) cannot be classified → leave allowed.
            std::size_t i = 0;
            std::vector<Script> found;
            bool valid = true;
            while (i < piece.size()) {
                const unsigned char c = static_cast<unsigned char>(piece[i]);
                std::size_t l = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
                if (l == 0 || i + l > piece.size()) { valid = false; break; }
                const Script s = scriptOf(decodeUtf8(piece, i));
                if (s != Script::Other && std::find(found.begin(), found.end(), s) == found.end()) found.push_back(s);
            }
            if (valid) tokenScripts[static_cast<std::size_t>(id)] = found;
        }
    }

    llama_sampler* samplerFor(const std::string& tgt) {
        auto it = samplers.find(tgt);
        if (it != samplers.end()) return it->second;

        std::vector<llama_logit_bias> biases;
        const bool known = tgt == "ko" || tgt == "ja" || tgt == "zh" || tgt == "th" || tgt == "en" || tgt == "es" || tgt == "vi";
        if (known) {
            for (std::size_t id = 0; id < tokenScripts.size(); ++id) {
                const auto& scripts = tokenScripts[id];
                if (scripts.empty()) continue;
                const auto tok = static_cast<llama_token>(id);
                // Never touch control / end-of-turn tokens — biasing them makes generation run on.
                if (llama_vocab_is_control(vocab, tok) || llama_vocab_is_eog(vocab, tok)) continue;
                bool foreign = false;
                for (const Script s : scripts)
                    if (!scriptAllowed(s, tgt)) foreign = true;
                if (foreign) biases.push_back({tok, -INFINITY});
            }
        }

        llama_sampler_chain_params sp = llama_sampler_chain_default_params();
        sp.no_perf = true;
        llama_sampler* chain = llama_sampler_chain_init(sp);
        if (!biases.empty())
            llama_sampler_chain_add(chain, llama_sampler_init_logit_bias(llama_vocab_n_tokens(vocab), static_cast<int32_t>(biases.size()), biases.data()));
        // Mild repetition penalty over the last 64 tokens: stops "ครับ ครับ ครับ" loops.
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(64, 1.15f, 0.0f, 0.0f));
        if (opts.temperature > 0.0f) {
            llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.9f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_temp(opts.temperature));
            llama_sampler_chain_add(chain, llama_sampler_init_dist(42));
        } else {
            llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        }
        samplers[tgt] = chain;
        return chain;
    }

    void freeSamplers() {
        for (auto& kv : samplers) llama_sampler_free(kv.second);
        samplers.clear();
    }
#endif
    LlmOptions opts;
    std::string path;
    std::mutex mutex;
};

LlmEngine::LlmEngine() : impl_(new Impl) {}
LlmEngine::~LlmEngine() { unload(); }

bool LlmEngine::isNativeLlama() {
#if TRANSLATOR_HAS_LLAMA
    return true;
#else
    return false;
#endif
}

bool LlmEngine::isLoaded() const {
#if TRANSLATOR_HAS_LLAMA
    return impl_->ctx != nullptr;
#else
    return !impl_->path.empty();
#endif
}

#if TRANSLATOR_HAS_LLAMA
namespace {
void llamaQuietLog(enum ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR && text) std::fputs(text, stderr);
}
} // namespace
#endif

bool LlmEngine::load(const std::string& ggufPath, const LlmOptions& opts, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    unload();
    impl_->opts = opts;
#if TRANSLATOR_HAS_LLAMA
    static std::once_flag backendOnce;
    std::call_once(backendOnce, [] {
        llama_log_set(llamaQuietLog, nullptr);
        llama_backend_init();
    });

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = opts.gpuLayers;
    mp.use_mmap = opts.useMmap;
    impl_->model = llama_model_load_from_file(ggufPath.c_str(), mp);
    if (!impl_->model) {
        if (error) *error = "llama: failed to load model: " + ggufPath;
        return false;
    }
    impl_->vocab = llama_model_get_vocab(impl_->model);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(std::max(512, opts.contextSize));
    cp.n_batch = cp.n_ctx;
    cp.n_ubatch = std::min<uint32_t>(cp.n_ctx, 512);
    cp.n_threads = opts.nThreads > 0 ? opts.nThreads : 4;
    cp.n_threads_batch = cp.n_threads;
    cp.no_perf = true;
    impl_->ctx = llama_init_from_model(impl_->model, cp);
    if (!impl_->ctx) {
        llama_model_free(impl_->model);
        impl_->model = nullptr;
        if (error) *error = "llama: failed to create context";
        return false;
    }

    impl_->classifyVocab();

    const char* tmpl = llama_model_chat_template(impl_->model, nullptr);
    impl_->chatTemplate = tmpl ? tmpl : "";
#endif
    impl_->path = ggufPath;
    if (error) error->clear();
    return true;
}

void LlmEngine::unload() {
#if TRANSLATOR_HAS_LLAMA
    impl_->freeSamplers();
    impl_->tokenScripts.clear();
    if (impl_->ctx) { llama_free(impl_->ctx); impl_->ctx = nullptr; }
    if (impl_->model) { llama_model_free(impl_->model); impl_->model = nullptr; }
    impl_->vocab = nullptr;
#endif
    impl_->path.clear();
}

LlmResult LlmEngine::translate(const std::string& text, const std::string& src, const std::string& tgt) {
    LlmResult r;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string input = trim(text);
    if (input.empty()) {
        r.ok = true;
        return r;
    }
#if !TRANSLATOR_HAS_LLAMA
    (void)src; (void)tgt;
    r.error = "llama.cpp not compiled in (TRANSLATOR_HAS_LLAMA=0)";
    return r;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ctx) {
        r.error = "llm not loaded";
        return r;
    }

    // Generation is attempted twice at most: the second time with an extra reminder when the
    // first output mixed in a foreign script (small models occasionally answer Thai in Chinese).
    auto generate = [&](bool strict, std::string& out, std::string& err) -> bool {
        // 1. Chat prompt with the model's own template (ChatML for Qwen, ...): system instruction,
        //    a one-shot demonstration in the right script, then the user's text.
        std::string system = buildInstruction(src, tgt, impl_->opts.systemPrompt);
        if (strict) system += " IMPORTANT: your previous answer mixed languages. Answer in " + languageName(tgt) + " only.";
        // Demonstrations are complete before any pointer into them is taken below.
        const std::string exLang = (src.empty() || src == "auto") ? "en" : src;
        std::vector<std::pair<std::string, std::string>> shots;
        if (exLang != tgt) {
            if (impl_->opts.examples == LlmExamples::Single) {
                std::string a = exampleSentence(exLang), b = exampleSentence(tgt);
                if (!a.empty() && !b.empty()) shots.emplace_back(std::move(a), std::move(b));
            } else if (impl_->opts.examples == LlmExamples::Diverse) {
                const std::vector<std::string> a = exampleSet(exLang), b = exampleSet(tgt);
                if (!a.empty() && a.size() == b.size())
                    for (std::size_t k = 0; k < a.size(); ++k) shots.emplace_back(a[k], b[k]);
            }
        }
        std::vector<llama_chat_message> msgs;
        msgs.push_back({"system", system.c_str()});
        std::size_t promptChars = system.size() + input.size() + 512;
        for (const auto& shot : shots) {
            msgs.push_back({"user", shot.first.c_str()});
            msgs.push_back({"assistant", shot.second.c_str()});
            promptChars += shot.first.size() + shot.second.size() + 64;
        }
        msgs.push_back({"user", input.c_str()});
        std::string prompt;
        const char* tmpl = impl_->chatTemplate.empty() ? nullptr : impl_->chatTemplate.c_str();
        std::vector<char> buf(promptChars);
        int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
        if (n > static_cast<int>(buf.size())) {
            buf.resize(static_cast<std::size_t>(n) + 1);
            n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
        }
        if (n < 0) {
            prompt = system + "\n\nText: " + input + "\nTranslation:";
        } else {
            prompt.assign(buf.data(), static_cast<std::size_t>(n));
        }

        // 2. Tokenize.
        std::vector<llama_token> tokens(prompt.size() + 16);
        int nTok = llama_tokenize(impl_->vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                                  static_cast<int32_t>(tokens.size()), true, true);
        if (nTok < 0) {
            tokens.resize(static_cast<std::size_t>(-nTok));
            nTok = llama_tokenize(impl_->vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                                  static_cast<int32_t>(tokens.size()), true, true);
        }
        if (nTok <= 0) {
            err = "llama: tokenization failed";
            return false;
        }
        tokens.resize(static_cast<std::size_t>(nTok));
        r.promptTokens = nTok;

        const int nCtx = static_cast<int>(llama_n_ctx(impl_->ctx));
        int maxOut = std::min(impl_->opts.maxOutputTokens, nCtx - nTok - 4);
        // Translations are rarely longer than ~2.5× the input (script changes) + headroom.
        maxOut = std::min(maxOut, static_cast<int>(input.size() / 2) + 64);
        if (maxOut <= 0) {
            err = "llama: prompt exceeds context size";
            return false;
        }

        // 3. Prompt processing.
        llama_kv_self_clear(impl_->ctx);
        llama_batch batch = llama_batch_get_one(tokens.data(), nTok);
        if (llama_decode(impl_->ctx, batch) != 0) {
            err = "llama: prompt decode failed";
            return false;
        }

        // 4. Greedy generation until EOS / cap, with the target's script constraints.
        llama_sampler* sampler = impl_->samplerFor(tgt);
        out.clear();
        char piece[256];
        const bool multiline = input.find('\n') != std::string::npos;
        llama_token prevTok = -1;
        int sameRun = 0;
        for (int i = 0; i < maxOut; ++i) {
            const llama_token tok = llama_sampler_sample(sampler, impl_->ctx, -1);
            if (llama_vocab_is_eog(impl_->vocab, tok)) break;
            const int len = llama_token_to_piece(impl_->vocab, tok, piece, sizeof(piece), 0, true);
            if (len > 0) out.append(piece, static_cast<std::size_t>(len));
            ++r.outputTokens;
            // A translation is one paragraph: a line break means a note/explanation is starting.
            if (!multiline && out.find('\n') != std::string::npos) break;
            // The same token four times in a row is a loop.
            sameRun = tok == prevTok ? sameRun + 1 : 0;
            prevTok = tok;
            if (sameRun >= 3) break;
            llama_token next = tok;
            batch = llama_batch_get_one(&next, 1);
            if (llama_decode(impl_->ctx, batch) != 0) {
                err = "llama: decode failed";
                return false;
            }
        }
        llama_sampler_reset(sampler);
        out = cleanOutput(out);
        return true;
    };

    std::string out, err;
    if (!generate(false, out, err)) {
        r.error = err;
        return r;
    }
    // Small models sometimes drift into another language mid-sentence; retry once, strictly.
    if (foreignScriptRatio(out, tgt) > 0.15) {
        std::string retry;
        if (generate(true, retry, err) && foreignScriptRatio(retry, tgt) < foreignScriptRatio(out, tgt)) out = retry;
        r.retried = true;
    }

    r.text = out;
    r.ok = true;
    r.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
#endif
}

} // namespace translator
