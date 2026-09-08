#include "translator/LlmEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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

std::string LlmEngine::buildInstruction(const std::string& src, const std::string& tgt, const std::string& systemPrompt) {
    if (!systemPrompt.empty()) return systemPrompt;
    const std::string target = languageName(tgt);
    std::string s = "You are a professional interpreter. ";
    if (src.empty() || src == "auto") s += "Detect the language of the user's message and translate it into " + target + ". ";
    else s += "Translate the user's message from " + languageName(src) + " into " + target + ". ";
    s += "Rules: output ONLY the " + target + " translation, nothing else; no explanations, no notes, no quotes; "
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
    llama_sampler* sampler = nullptr;
    std::string chatTemplate;
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

    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    impl_->sampler = llama_sampler_chain_init(sp);
    if (opts.temperature > 0.0f) {
        llama_sampler_chain_add(impl_->sampler, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(impl_->sampler, llama_sampler_init_temp(opts.temperature));
        llama_sampler_chain_add(impl_->sampler, llama_sampler_init_dist(42));
    } else {
        llama_sampler_chain_add(impl_->sampler, llama_sampler_init_greedy());
    }

    const char* tmpl = llama_model_chat_template(impl_->model, nullptr);
    impl_->chatTemplate = tmpl ? tmpl : "";
#endif
    impl_->path = ggufPath;
    if (error) error->clear();
    return true;
}

void LlmEngine::unload() {
#if TRANSLATOR_HAS_LLAMA
    if (impl_->sampler) { llama_sampler_free(impl_->sampler); impl_->sampler = nullptr; }
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

    // 1. Build the chat prompt with the model's own template (ChatML for Qwen, etc.).
    const std::string system = buildInstruction(src, tgt, impl_->opts.systemPrompt);
    std::vector<llama_chat_message> msgs = {{"system", system.c_str()}, {"user", input.c_str()}};
    std::string prompt;
    const char* tmpl = impl_->chatTemplate.empty() ? nullptr : impl_->chatTemplate.c_str();
    std::vector<char> buf(system.size() + input.size() + 512);
    int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
    if (n > static_cast<int>(buf.size())) {
        buf.resize(static_cast<std::size_t>(n) + 1);
        n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
    }
    if (n < 0) {
        // No usable template: fall back to a plain instruction format.
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
        r.error = "llama: tokenization failed";
        return r;
    }
    tokens.resize(static_cast<std::size_t>(nTok));
    r.promptTokens = nTok;

    const int nCtx = static_cast<int>(llama_n_ctx(impl_->ctx));
    int maxOut = std::min(impl_->opts.maxOutputTokens, nCtx - nTok - 4);
    // Translations are rarely longer than ~2.5× the input (script changes) + headroom.
    maxOut = std::min(maxOut, static_cast<int>(input.size() / 2) + 64);
    if (maxOut <= 0) {
        r.error = "llama: prompt exceeds context size";
        return r;
    }

    // 3. Prompt processing.
    llama_kv_self_clear(impl_->ctx);
    llama_batch batch = llama_batch_get_one(tokens.data(), nTok);
    if (llama_decode(impl_->ctx, batch) != 0) {
        r.error = "llama: prompt decode failed";
        return r;
    }

    // 4. Greedy generation until EOS / cap.
    std::string out;
    char piece[256];
    for (int i = 0; i < maxOut; ++i) {
        const llama_token tok = llama_sampler_sample(impl_->sampler, impl_->ctx, -1);
        if (llama_vocab_is_eog(impl_->vocab, tok)) break;
        const int len = llama_token_to_piece(impl_->vocab, tok, piece, sizeof(piece), 0, true);
        if (len > 0) out.append(piece, static_cast<std::size_t>(len));
        ++r.outputTokens;
        // Stop early on a line break after content: the model is starting a note/explanation.
        if (out.size() > 2 && out.find('\n') != std::string::npos && out.find("\n\n") != std::string::npos) break;
        llama_token next = tok;
        batch = llama_batch_get_one(&next, 1);
        if (llama_decode(impl_->ctx, batch) != 0) {
            r.error = "llama: decode failed";
            return r;
        }
    }
    llama_sampler_reset(impl_->sampler);

    r.text = cleanOutput(out);
    r.ok = true;
    r.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
#endif
}

} // namespace translator
