#include "translator/TranslationPipeline.hpp"

#include "translator/Denoiser.hpp"
#include "translator/FileUtil.hpp"
#include "translator/NmtEngine.hpp"
#include "translator/SttEngine.hpp"

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
    SpeechSegmenter segmenter;

    std::vector<float> frameBuf;       // partial frame carry-over for feedAudio
    float frameOut[kFrameSize] = {};

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

    impl_->nmt.init(cfg.nmtRootDir, cfg.nThreads, cfg.beamSize, cfg.maxDecodingLength);
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
    SttResult r = impl_->stt.transcribe(pcm, n, sourceLang, impl_->cfg.initialPrompt);
    if (!r.ok) impl_->setError(r.error);
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
    if (sourceLang.empty() || sourceLang == "auto") {
        r.error = "source language must be known for text translation";
        impl_->setError(r.error);
        return r;
    }
    std::string err;
    if (!impl_->nmt.translate(text, sourceLang, targetLang, impl_->cfg.pivotLangs, r.translatedText, &r.route, &err)) {
        r.error = err;
        impl_->setError(err);
        return r;
    }
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
    const SttResult stt = impl_->stt.transcribe(pcm, n, sourceLang, impl_->cfg.initialPrompt);
    r.sttMs = stt.elapsedMs;
    if (!stt.ok) {
        r.error = stt.error;
        impl_->setError(r.error);
        return r;
    }
    r.sourceText = stt.text;
    if (!stt.detectedLang.empty()) r.sourceLang = stt.detectedLang;
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
        std::string err;
        if (!impl_->nmt.translate(r.sourceText, r.sourceLang, targetLang, impl_->cfg.pivotLangs, r.translatedText, &r.route, &err)) {
            r.error = err;
            impl_->setError(err);
            r.nmtMs = Impl::msSince(t1);
            r.totalMs = Impl::msSince(t0);
            return r;
        }
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
    return !impl_->nmt.resolveRoute(src, tgt, impl_->cfg.pivotLangs).empty();
}

Json TranslationPipeline::capabilities() const {
    Json j = Json::object();
    j.set("rnnoise", impl_->denoiser.isNativeRnnoise());
    j.set("whisper", impl_->stt.isNativeWhisper());
    j.set("ctranslate2", impl_->nmt.isNativeCTranslate2());
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
