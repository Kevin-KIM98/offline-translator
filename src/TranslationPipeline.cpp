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
            // A small LLM translates from English far better than from Korean or Japanese. Into Thai
            // it got 19 of 30 conversational sentences right from Marian's English against 11 of 30
            // straight from Korean, so reach English with a dedicated model first when one exists.
            if (cfg.backend == TranslationBackend::Auto && cfg.llmPivotThroughEnglish && !src.empty() && src != "auto" &&
                src != "en" && tgt != "en" && !nmt.resolveRoute(src, "en", cfg.pivotLangs).empty()) {
                std::string english, hopError;
                std::vector<std::string> hops;
                if (nmt.translate(text, src, "en", cfg.pivotLangs, english, &hops, &hopError) && !english.empty()) {
                    const LlmResult viaEnglish = llm.translate(english, "en", tgt);
                    if (viaEnglish.ok) {
                        r.translatedText = viaEnglish.text;
                        r.route = hops;
                        r.route.push_back("llm");
                        return true;
                    }
                }
                // Either hop failed: the LLM still gets a chance with the original text below.
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
        // "auto" reaches here only when detection failed; fall back to the previous language.
        const std::string promptLang = (lang.empty() || lang == "auto") ? previousLang : lang;
        o.initialPrompt = text::buildSttPrompt(wantDefault ? std::string() : cfg.initialPrompt,
                                               wantDefault ? promptLang : std::string(),
                                               cfg.useContextPrompt ? previousTranscript : std::string());
        return o;
    }

    // "auto": detect the language first so the per-language prompt applies. Whisper's own auto
    // mode decodes without one, which on the test set cost Chinese 1.2% → 7.8% CER (it drifted
    // into traditional characters). The detection pass encodes the same reduced window whisper
    // would have encoded for its own detection, so the extra cost is one mel computation.
    std::string resolveLang(const float* pcm, std::size_t n, const std::string& lang) {
        if (!lang.empty() && lang != "auto") return lang;
        const std::string detected = stt.detectLanguage(pcm, n, cfg.sttAdaptiveAudioContext);
        return detected.empty() ? lang : detected;
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
        lo.examples = cfg.llmExamples;
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

    // RNNoise supplies the voice probability and the level the loudness gate compares; whisper
    // hears the microphone audio itself unless sttOnDenoisedAudio asks for the denoised frames.
    // With the denoiser off an energy gate stands in as the VAD: a constant "voiced" would put
    // every pause into the utterance and the loudness gate's median would drop it.
    auto push = [&](const float* frame) {
        if (!impl_->cfg.enableDenoise) return impl_->segmenter.pushFrame(frame, impl_->denoiser.energyVad(frame));
        const float vad = impl_->denoiser.process(frame, impl_->frameOut);
        return impl_->cfg.sttOnDenoisedAudio ? impl_->segmenter.pushFrame(impl_->frameOut, vad)
                                             : impl_->segmenter.pushFrame(frame, vad, impl_->frameOut);
    };

    // Complete a partially buffered frame first.
    if (!impl_->frameBuf.empty()) {
        const std::size_t need = kFrameSize - impl_->frameBuf.size();
        const std::size_t take = std::min(need, n);
        impl_->frameBuf.insert(impl_->frameBuf.end(), pcm, pcm + take);
        i = take;
        if (impl_->frameBuf.size() < static_cast<std::size_t>(kFrameSize)) return false;
        ready |= push(impl_->frameBuf.data());
        impl_->frameBuf.clear();
    }

    for (; i + kFrameSize <= n; i += kFrameSize) ready |= push(pcm + i);
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
    const auto t0 = std::chrono::steady_clock::now();
    const std::string lang = impl_->resolveLang(pcm, n, sourceLang);
    const double detectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    SttResult r = impl_->stt.transcribe(pcm, n, lang, impl_->decodeOptions(lang));
    if (!r.ok) {
        impl_->setError(r.error);
        return r;
    }
    r.elapsedMs += detectMs;   // "auto" pays for the detection pass; report it as STT time
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

    // 1. STT ("auto" pays for a detection pass first; it counts as STT time)
    const std::string lang = impl_->resolveLang(pcm, n, sourceLang);
    const double detectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const SttResult stt = impl_->stt.transcribe(pcm, n, lang, impl_->decodeOptions(lang));
    r.sttMs = stt.elapsedMs + detectMs;
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
