// End-to-end engine: RNNoise → SpeechSegmenter → whisper.cpp → CTranslate2.
// TTS is deliberately left to the OS layer (AVSpeechSynthesizer / android.speech.tts).
//
// Two usage modes:
//   1. Streaming: feedAudio() continuously from the microphone; when it returns true call
//      processPendingUtterance() (on a worker thread) to get a TranslationResult.
//   2. One-shot: processSpeechToTranslation() on a complete buffer (push-to-talk / files).
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "translator/SpeechSegmenter.hpp"
#include "translator/Types.hpp"

namespace translator {

class TranslationPipeline {
public:
    TranslationPipeline();
    ~TranslationPipeline();
    TranslationPipeline(const TranslationPipeline&) = delete;
    TranslationPipeline& operator=(const TranslationPipeline&) = delete;

    bool initialize(const PipelineConfig& cfg, std::string* error = nullptr);
    bool isInitialized() const;
    const PipelineConfig& config() const;
    void shutdown();

    // ---- Streaming front-end (call from the audio thread) ----------------------
    // Denoises exactly one 480-sample frame. Returns VAD probability.
    float denoiseFrame(const float* in480, float* out480);

    // Accepts any number of samples (buffered to 480-sample frames internally).
    // Returns true when at least one utterance is ready.
    bool feedAudio(const float* pcm, std::size_t n);
    bool hasPendingUtterance() const;
    std::size_t pendingUtteranceCount() const;
    // Force-close the current utterance (push-to-talk release). Returns true if one was emitted.
    bool flushAudio();
    void resetAudio();
    void setSegmenterConfig(const SegmenterConfig& cfg);
    const SegmenterConfig& segmenterConfig() const;

    // Pops the oldest utterance and runs STT + NMT on it (blocking; run off the audio thread).
    TranslationResult processPendingUtterance(const std::string& sourceLang, const std::string& targetLang);
    // Pops the oldest utterance without processing it (e.g. to hand the audio to a caller).
    std::vector<float> popPendingUtterance();

    // ---- One-shot API ----------------------------------------------------------
    SttResult transcribe(const float* pcm, std::size_t n, const std::string& sourceLang);
    TranslationResult translateText(const std::string& text, const std::string& sourceLang, const std::string& targetLang);
    TranslationResult processSpeechToTranslation(const float* pcm, std::size_t n,
                                                 const std::string& sourceLang, const std::string& targetLang);

    // ---- NMT management --------------------------------------------------------
    bool preloadPair(const std::string& src, const std::string& tgt, std::string* error = nullptr);
    void unloadPair(const std::string& src, const std::string& tgt);
    std::vector<std::string> availablePairs() const;
    bool canTranslate(const std::string& src, const std::string& tgt) const;

    // Build info: which native engines are compiled in.
    Json capabilities() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace translator
