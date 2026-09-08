#include "translator/TranslationPipeline.hpp"

#include "translator/Denoiser.hpp"
#include "translator/FileUtil.hpp"
#include "translator/LlmEngine.hpp"
#include "translator/NmtEngine.hpp"
#include "translator/SttEngine.hpp"
#include "translator/TextUtil.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

namespace translator {

struct TranslationPipeline::Impl {
    PipelineConfig cfg;
    bool initialized = false;
    std::string lastError;

    Denoiser denoiser;
    SttEngine stt;
    NmtEngine nmt;
    LlmEngine llm;
    SpeechSegmenter segmenter;

    // Runs the configured backend. Fills translatedText/route or error.
    bool translateWith(const std::string& text, const std::string& src, const std::string& tgt, TranslationResult& r) {
        std::string err;
        const bool llmReady = llm.isLoaded();
        const bool marianRoute = (src != "auto" && !src.empty()) && !nmt.resolveRoute(src, tgt, cfg.pivotLangs).empty();
        bool useLlm = false;
        switch (cfg.backend) {
            case TranslationBackend::Llm: useLlm = true; break;
            case TranslationBackend::Marian: useLlm = false; break;
            case TranslationBackend::Auto: useLlm = !marianRoute && llmReady; break;
        }
        if (useLlm) {
            if (!llmReady) {
                r.error = "LLM backend requested but no LLM model loaded";
                return false;
            }
            const LlmResult lr = llm.translate(text, src, tgt);
            if (!lr.ok) {
                r.error = lr.error;
                return false;
            }
            r.translatedText = lr.text;
            r.route = {"llm"};
            return true;
        }
        if (src.empty() || src == "auto") {
            r.error = "source language must be known for Marian translation (load an LLM model for auto-detect)";
            return false;
        }
        if (!nmt.translate(text, src, tgt, cfg.pivotLangs, r.translatedText, &r.route, &err)) {
            if (llmReady && cfg.backend == TranslationBackend::Auto) {
                const LlmResult lr = llm.translate(text, src, tgt);
                if (lr.ok) {
                    r.translatedText = lr.text;
                    r.route = {"llm"};
                    return true;
                }
            }
            r.error = err;
            return false;
        }
        return true;
    }

    std::vector<float> frameBuf;       // partial frame carry-over for feedAudio
    float frameOut[kFrameSize] = {};

    std::string previousTranscript;    // conversation context for the next whisper prompt
    std::string previousLang;

    SttEngine::DecodeOptions decodeOptions(const std::string& lang) const {
        SttEngine::DecodeOptions o;
        o.beamSize = cfg.sttBeamSize;
        o.noSpeechThreshold = cfg.noSpeechThreshold;
        o.adaptiveAudioContext = cfg.sttAdaptiveAudioContext;
        const bool wantDefault = cfg.initialPrompt.empty() && cfg.useDefaultPrompts;
        // With "auto" we only know the language after the first utterance; reuse it.
        const std::string promptLang = (lang.empty() || lang == "auto") ? previousLang : lang;
        o.initialPrompt = text::buildSttPrompt(wantDefault ? std::string() : cfg.initialPrompt,
                                               wantDefault ? promptLang : std::string(),
                                               cfg.useContextPrompt ? previousTranscript : std::string());
        return o;
    }

    void rememberTranscript(const std::string& t, const std::string& lang) {
        if (t.empty()) return;
        if (!lang.empty() && lang != previousLang) previousTranscript.clear(); // language switched
        previousLang = lang;
        previousTranscript = previousTranscript.empty() ? t : previousTranscript + " " + t;
        if (previousTranscript.size() > 600) previousTranscript = previousTranscript.substr(previousTranscript.size() - 600);
    }

    mutable std::mutex audioMutex;     // segmenter + denoiser + frameBuf
    mutable std::mutex engineMutex;    // stt + nmt (serializes inference)
    mutable std::mutex errorMutex;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = e;
    }

    static double msSince(const std::chrono::steady_clock::time_point& t0) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};

TranslationPipeline::TranslationPipeline() : impl_(new Impl) {}
TranslationPipeline::~TranslationPipeline() { shutdown(); }

bool TranslationPipeline::isInitialized() const { return impl_->initialized; }
const PipelineConfig& TranslationPipeline::config() const { return impl_->cfg; }
std::string TranslationPipeline::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->lastError;
}

bool TranslationPipeline::initialize(const PipelineConfig& cfg, std::string* error) {
    std::lock_guard<std::mutex> engineLock(impl_->engineMutex);
    std::lock_guard<std::mutex> audioLock(impl_->audioMutex);

    impl_->cfg = cfg;
    impl_->initialized = false;
    std::string err;

    if (cfg.enableDenoise && !impl_->denoiser.init()) {
        err = "RNNoise initialization failed";
        impl_->setError(err);
        if (error) *error = err;
        return false;
    }

    if (!cfg.whisperModelPath.empty()) {
        if (!fs::isFile(cfg.whisperModelPath)) {
            err = "whisper model file not found: " + cfg.whisperModelPath;
            impl_->setError(err);
            if (error) *error = err;
            return false;
        }
        if (!impl_->stt.load(cfg.whisperModelPath, cfg.useGpu, cfg.nThreads, &err)) {
            impl_->setError(err);
            if (error) *error = err;
            return false;
        }
    }

    NmtEngine::Options nmtOpts;
    nmtOpts.nThreads = cfg.nThreads;
    nmtOpts.beamSize = cfg.beamSize;
    nmtOpts.maxDecodingLength = cfg.maxDecodingLength;
    nmtOpts.noRepeatNgramSize = cfg.noRepeatNgramSize;
    nmtOpts.repetitionPenalty = cfg.repetitionPenalty;
    impl_->nmt.init(cfg.nmtRootDir, nmtOpts);
    impl_->previousTranscript.clear();
    impl_->previousLang.clear();

    impl_->llm.unload();
    if (!cfg.llmModelPath.empty()) {
        if (!fs::isFile(cfg.llmModelPath)) {
            err = "LLM model file not found: " + cfg.llmModelPath;
            impl_->setError(err);
            if (error) *error = err;
            return false;
        }
        LlmOptions lo;
        lo.nThreads = cfg.nThreads;
        lo.contextSize = cfg.llmContextSize;
        lo.maxOutputTokens = cfg.llmMaxOutputTokens;
        lo.temperature = cfg.llmTemperature;
        lo.gpuLayers = cfg.llmGpuLayers;
        lo.systemPrompt = cfg.llmSystemPrompt;
        if (!impl_->llm.load(cfg.llmModelPath, lo, &err)) {
            impl_->setError(err);
            if (error) *error = err;
            return false;
        }
    } else if (cfg.backend == TranslationBackend::Llm) {
        err = "backend=llm requires llmModelPath";
        impl_->setError(err);
        if (error) *error = err;
        return false;
    }
    if (cfg.preloadAllPairs) {
        for (const auto& pair : impl_->nmt.availablePairs()) {
            const auto dash = pair.find('-');
            impl_->nmt.loadPair(pair.substr(0, dash), pair.substr(dash + 1), &err);
        }
    }

    impl_->segmenter.reset();
    impl_->frameBuf.clear();
    impl_->initialized = true;
    if (error) error->clear();
    return true;
}

void TranslationPipeline::shutdown() {
    std::lock_guard<std::mutex> engineLock(impl_->engineMutex);
    std::lock_guard<std::mutex> audioLock(impl_->audioMutex);
    impl_->stt.unload();
    impl_->nmt.unloadAll();
    impl_->llm.unload();
    impl_->segmenter.reset();
    impl_->frameBuf.clear();
    impl_->initialized = false;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

float TranslationPipeline::denoiseFrame(const float* in480, float* out480) {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    if (!impl_->cfg.enableDenoise) {
        if (in480 != out480) std::memcpy(out480, in480, sizeof(float) * kFrameSize);
        return 1.0f; // no VAD available → treat as speech
    }
    return impl_->denoiser.process(in480, out480);
}

bool TranslationPipeline::feedAudio(const float* pcm, std::size_t n) {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    bool ready = false;
    std::size_t i = 0;

    // Complete a partially buffered frame first.
    if (!impl_->frameBuf.empty()) {
        const std::size_t need = kFrameSize - impl_->frameBuf.size();
        const std::size_t take = std::min(need, n);
        impl_->frameBuf.insert(impl_->frameBuf.end(), pcm, pcm + take);
        i = take;
        if (impl_->frameBuf.size() < static_cast<std::size_t>(kFrameSize)) return false;
        const float vad = impl_->cfg.enableDenoise ? impl_->denoiser.process(impl_->frameBuf.data(), impl_->frameOut)
                                                   : (std::memcpy(impl_->frameOut, impl_->frameBuf.data(), sizeof(impl_->frameOut)), 1.0f);
        ready |= impl_->segmenter.pushFrame(impl_->frameOut, vad);
        impl_->frameBuf.clear();
    }

    for (; i + kFrameSize <= n; i += kFrameSize) {
        const float vad = impl_->cfg.enableDenoise ? impl_->denoiser.process(pcm + i, impl_->frameOut)
                                                   : (std::memcpy(impl_->frameOut, pcm + i, sizeof(impl_->frameOut)), 1.0f);
        ready |= impl_->segmenter.pushFrame(impl_->frameOut, vad);
    }
    if (i < n) impl_->frameBuf.assign(pcm + i, pcm + n);
    return ready;
}

bool TranslationPipeline::hasPendingUtterance() const {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    return impl_->segmenter.hasUtterance();
}

std::size_t TranslationPipeline::pendingUtteranceCount() const {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    return impl_->segmenter.pendingCount();
}

bool TranslationPipeline::flushAudio() {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    impl_->frameBuf.clear();
    return impl_->segmenter.flush();
}

void TranslationPipeline::resetAudio() {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    impl_->frameBuf.clear();
    impl_->segmenter.reset();
    impl_->denoiser.reset();
    impl_->previousTranscript.clear();
}

void TranslationPipeline::setSegmenterConfig(const SegmenterConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    impl_->segmenter = SpeechSegmenter(cfg);
}

const SegmenterConfig& TranslationPipeline::segmenterConfig() const { return impl_->segmenter.config(); }

std::vector<float> TranslationPipeline::popPendingUtterance() {
    std::lock_guard<std::mutex> lock(impl_->audioMutex);
    return impl_->segmenter.popUtterance();
}

TranslationResult TranslationPipeline::processPendingUtterance(const std::string& sourceLang, const std::string& targetLang) {
    const std::vector<float> audio = popPendingUtterance();
    if (audio.empty()) {
        TranslationResult r;
        r.error = "no pending utterance";
        r.targetLang = targetLang;
        return r;
    }
    return processSpeechToTranslation(audio.data(), audio.size(), sourceLang, targetLang);
}

// ---------------------------------------------------------------------------
// One-shot
// ---------------------------------------------------------------------------

SttResult TranslationPipeline::transcribe(const float* pcm, std::size_t n, const std::string& sourceLang) {
    std::lock_guard<std::mutex> lock(impl_->engineMutex);
    if (!impl_->initialized) {
        SttResult r;
        r.error = "pipeline not initialized";
        return r;
    }
    SttResult r = impl_->stt.transcribe(pcm, n, sourceLang, impl_->decodeOptions(sourceLang));
    if (!r.ok) {
        impl_->setError(r.error);
        return r;
    }
    if (impl_->cfg.cleanTranscripts) r.text = text::cleanTranscript(r.text, r.detectedLang);
    impl_->rememberTranscript(r.text, r.detectedLang);
    return r;
}

TranslationResult TranslationPipeline::translateText(const std::string& text, const std::string& sourceLang, const std::string& targetLang) {
    TranslationResult r;
    r.sourceText = text;
    r.sourceLang = sourceLang;
    r.targetLang = targetLang;
    const auto t0 = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(impl_->engineMutex);
    if (!impl_->initialized) {
        r.error = "pipeline not initialized";
        return r;
    }
    if (sourceLang == targetLang) {
        r.translatedText = text;
    } else if (!impl_->translateWith(text, sourceLang, targetLang, r)) {
        impl_->setError(r.error);
        return r;
    }
    if (impl_->cfg.postProcessTranslations && !r.route.empty()) r.translatedText = text::postProcessTranslation(r.translatedText, targetLang);
    r.nmtMs = Impl::msSince(t0);
    r.totalMs = r.nmtMs;
    r.ok = true;
    return r;
}

TranslationResult TranslationPipeline::processSpeechToTranslation(const float* pcm, std::size_t n,
                                                                  const std::string& sourceLang, const std::string& targetLang) {
    TranslationResult r;
    r.sourceLang = sourceLang;
    r.targetLang = targetLang;
    const auto t0 = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(impl_->engineMutex);
    if (!impl_->initialized) {
        r.error = "pipeline not initialized";
        return r;
    }
    if (!pcm || n == 0) {
        r.error = "empty audio";
        return r;
    }

    // 1. STT
    const SttResult stt = impl_->stt.transcribe(pcm, n, sourceLang, impl_->decodeOptions(sourceLang));
    r.sttMs = stt.elapsedMs;
    if (!stt.ok) {
        r.error = stt.error;
        impl_->setError(r.error);
        return r;
    }
    if (!stt.detectedLang.empty()) r.sourceLang = stt.detectedLang;
    r.sourceText = impl_->cfg.cleanTranscripts ? text::cleanTranscript(stt.text, r.sourceLang) : stt.text;
    impl_->rememberTranscript(r.sourceText, r.sourceLang);
    if (r.sourceText.empty()) {
        // Silence / non-speech: not an error, just nothing to translate.
        r.ok = true;
        r.totalMs = Impl::msSince(t0);
        return r;
    }

    // 2. NMT
    const auto t1 = std::chrono::steady_clock::now();
    if (r.sourceLang == targetLang) {
        r.translatedText = r.sourceText;
    } else {
        if (!impl_->translateWith(r.sourceText, r.sourceLang, targetLang, r)) {
            impl_->setError(r.error);
            r.nmtMs = Impl::msSince(t1);
            r.totalMs = Impl::msSince(t0);
            return r;
        }
        if (impl_->cfg.postProcessTranslations) r.translatedText = text::postProcessTranslation(r.translatedText, targetLang);
    }
    r.nmtMs = Impl::msSince(t1);
    r.totalMs = Impl::msSince(t0);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// NMT management / info
// ---------------------------------------------------------------------------

bool TranslationPipeline::preloadPair(const std::string& src, const std::string& tgt, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->engineMutex);
    std::string err;
    const bool ok = impl_->nmt.loadPair(src, tgt, &err);
    if (!ok) impl_->setError(err);
    if (error) *error = err;
    return ok;
}

void TranslationPipeline::unloadPair(const std::string& src, const std::string& tgt) {
    std::lock_guard<std::mutex> lock(impl_->engineMutex);
    impl_->nmt.unloadPair(src, tgt);
}

std::vector<std::string> TranslationPipeline::availablePairs() const { return impl_->nmt.availablePairs(); }

bool TranslationPipeline::canTranslate(const std::string& src, const std::string& tgt) const {
    if (src == tgt) return true;
    if (impl_->cfg.backend != TranslationBackend::Marian && impl_->llm.isLoaded()) return true;
    if (impl_->cfg.backend == TranslationBackend::Llm) return false;
    return !impl_->nmt.resolveRoute(src, tgt, impl_->cfg.pivotLangs).empty();
}

Json TranslationPipeline::capabilities() const {
    Json j = Json::object();
    j.set("rnnoise", impl_->denoiser.isNativeRnnoise());
    j.set("whisper", impl_->stt.isNativeWhisper());
    j.set("ctranslate2", impl_->nmt.isNativeCTranslate2());
    j.set("llama", LlmEngine::isNativeLlama());
    j.set("llm_loaded", impl_->llm.isLoaded());
    j.set("backend", impl_->cfg.backend == TranslationBackend::Llm ? "llm" : impl_->cfg.backend == TranslationBackend::Marian ? "marian" : "auto");
#if TRANSLATOR_HAS_SENTENCEPIECE
    j.set("sentencepiece", true);
#else
    j.set("sentencepiece", false);
#endif
    j.set("initialized", impl_->initialized);
    Json pairs = Json::array();
    for (const auto& p : impl_->nmt.availablePairs()) pairs.push_back(p);
    j.set("available_pairs", pairs);
    Json langs = Json::array();
    for (const auto& l : supportedLanguages()) langs.push_back(l);
    j.set("languages", langs);
    return j;
}

} // namespace translator
