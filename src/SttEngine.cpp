#include "translator/SttEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#if TRANSLATOR_HAS_WHISPER
#include "whisper.h"
#endif

namespace translator {

struct SttEngine::Impl {
#if TRANSLATOR_HAS_WHISPER
    whisper_context* ctx = nullptr;
#endif
    int nThreads = 4;
    std::string modelPath;
};

SttEngine::SttEngine() : impl_(new Impl) {}

SttEngine::~SttEngine() {
    unload();
    delete impl_;
}

bool SttEngine::isLoaded() const {
#if TRANSLATOR_HAS_WHISPER
    return impl_->ctx != nullptr;
#else
    return !impl_->modelPath.empty();
#endif
}

bool SttEngine::isNativeWhisper() const {
#if TRANSLATOR_HAS_WHISPER
    return true;
#else
    return false;
#endif
}

void SttEngine::unload() {
#if TRANSLATOR_HAS_WHISPER
    if (impl_->ctx) {
        whisper_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
#endif
    impl_->modelPath.clear();
}

#if TRANSLATOR_HAS_WHISPER
namespace {
void quietLog(enum ggml_log_level level, const char* text, void* /*user*/) {
    if (level == GGML_LOG_LEVEL_ERROR && text) std::fputs(text, stderr);
}
} // namespace
#endif

bool SttEngine::load(const std::string& modelPath, bool useGpu, int nThreads, std::string* error) {
    unload();
    impl_->nThreads = nThreads > 0 ? nThreads : 4;

#if TRANSLATOR_HAS_WHISPER
    whisper_log_set(quietLog, nullptr);

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = useGpu;
    impl_->ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!impl_->ctx) {
        if (error) *error = "whisper: failed to load model: " + modelPath;
        return false;
    }
#else
    (void)useGpu;
    if (error) error->clear();
#endif
    impl_->modelPath = modelPath;
    return true;
}

#if TRANSLATOR_HAS_WHISPER
namespace {
// whisper rejects input shorter than 1 s; pad with silence.
constexpr std::size_t kMinSamples = static_cast<std::size_t>(kSampleRate) * 11 / 10;

// 1500 encoder positions ↔ 30 s. Keep ≥ 1.5 s of headroom and a floor of 512 (~10 s);
// 0 (whisper's full window) for audio of 20 s and more.
int adaptiveAudioCtx(std::size_t samples) {
    const double seconds = static_cast<double>(samples) / kSampleRate;
    if (seconds >= 20.0) return 0;
    const int ctx = static_cast<int>(seconds * 50.0) + 96;
    return std::max(512, std::min(1500, ctx));
}
} // namespace
#endif

std::string SttEngine::detectLanguage(const float* pcm, std::size_t n, bool adaptiveAudioContext) {
#if !TRANSLATOR_HAS_WHISPER
    (void)pcm; (void)n; (void)adaptiveAudioContext;
    return std::string();
#else
    if (!isLoaded() || !pcm || n == 0) return std::string();
    std::vector<float> padded;
    const float* data = pcm;
    std::size_t count = n;
    if (n < kMinSamples) {
        padded.assign(pcm, pcm + n);
        padded.resize(kMinSamples, 0.0f);
        data = padded.data();
        count = padded.size();
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = impl_->nThreads;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.language = "auto";
    wparams.detect_language = true;     // whisper_full returns right after the detection pass
    if (adaptiveAudioContext) {
        const int ctx = adaptiveAudioCtx(count);
        if (ctx > 0) wparams.audio_ctx = ctx;   // honoured by the detection pass: see cmake/patches
    }
    if (whisper_full(impl_->ctx, wparams, data, static_cast<int>(count)) != 0) return std::string();
    const int langId = whisper_full_lang_id(impl_->ctx);
    const char* code = langId >= 0 ? whisper_lang_str(langId) : nullptr;
    return code ? std::string(code) : std::string();
#endif
}

SttResult SttEngine::transcribe(const float* pcm, std::size_t n, const std::string& lang, const DecodeOptions& opts) {
    SttResult r;
    const auto t0 = std::chrono::steady_clock::now();

#if !TRANSLATOR_HAS_WHISPER
    r.error = "whisper not compiled in (TRANSLATOR_HAS_WHISPER=0)";
    (void)pcm; (void)n; (void)lang; (void)opts; (void)t0;
    return r;
#else
    if (!isLoaded()) {
        r.error = "stt engine not loaded";
        return r;
    }
    if (!pcm || n == 0) {
        r.error = "empty audio";
        return r;
    }

    std::vector<float> padded;
    const float* data = pcm;
    std::size_t count = n;
    if (n < kMinSamples) {
        padded.assign(pcm, pcm + n);
        padded.resize(kMinSamples, 0.0f);
        data = padded.data();
        count = padded.size();
    }

    const bool beam = opts.beamSize > 1;
    whisper_full_params wparams = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (beam) wparams.beam_search.beam_size = opts.beamSize;
    else wparams.greedy.best_of = 1;
    wparams.n_threads = impl_->nThreads;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.translate = false;          // we never use whisper's built-in EN translation
    wparams.no_context = true;          // each utterance is independent
    wparams.single_segment = false;
    wparams.suppress_blank = true;
    wparams.suppress_nst = true;        // suppress non-speech tokens ("[Music]", "♪", ...)
    wparams.no_timestamps = true;
    wparams.temperature = 0.0f;
    wparams.temperature_inc = 0.2f;

    const std::string langBuf = (lang.empty() || lang == "auto") ? "auto" : lang;
    wparams.language = langBuf.c_str();
    wparams.detect_language = false;
    wparams.initial_prompt = opts.initialPrompt.empty() ? nullptr : opts.initialPrompt.c_str();

    if (opts.adaptiveAudioContext) {
        const int ctx = adaptiveAudioCtx(count);
        if (ctx > 0) wparams.audio_ctx = ctx;
    }

    if (whisper_full(impl_->ctx, wparams, data, static_cast<int>(count)) != 0) {
        r.error = "whisper_full failed";
        return r;
    }

    const int nSeg = whisper_full_n_segments(impl_->ctx);
    for (int i = 0; i < nSeg; ++i) {
        // Skip segments whisper itself considers non-speech (music, silence, breathing).
        if (opts.noSpeechThreshold > 0.0f && whisper_full_get_segment_no_speech_prob(impl_->ctx, i) > opts.noSpeechThreshold) continue;
        const char* seg = whisper_full_get_segment_text(impl_->ctx, i);
        if (seg) r.text += seg;
    }
    const int langId = whisper_full_lang_id(impl_->ctx);
    if (langId >= 0) {
        const char* code = whisper_lang_str(langId);
        if (code) r.detectedLang = code;
    }
    if (r.detectedLang.empty() && langBuf != "auto") r.detectedLang = langBuf;

    // Trim leading/trailing whitespace.
    const auto first = r.text.find_first_not_of(" \t\r\n");
    const auto last = r.text.find_last_not_of(" \t\r\n");
    r.text = (first == std::string::npos) ? std::string() : r.text.substr(first, last - first + 1);

    r.ok = true;
    r.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
#endif
}

} // namespace translator
