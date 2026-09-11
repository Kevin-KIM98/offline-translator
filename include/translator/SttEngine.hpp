// whisper.cpp wrapper. Loads one ggml model and transcribes 16 kHz mono float PCM.
#pragma once

#include <cstddef>
#include <string>

#include "translator/Types.hpp"

namespace translator {

class SttEngine {
public:
    SttEngine();
    ~SttEngine();
    SttEngine(const SttEngine&) = delete;
    SttEngine& operator=(const SttEngine&) = delete;

    // useGpu enables Metal / Vulkan / OpenCL when whisper.cpp was compiled with them.
    bool load(const std::string& modelPath, bool useGpu, int nThreads, std::string* error = nullptr);
    void unload();
    bool isLoaded() const;
    bool isNativeWhisper() const;

    struct DecodeOptions {
        int beamSize = 5;                 // 1 = greedy
        float noSpeechThreshold = 0.85f;  // segments above this no-speech probability are dropped
        std::string initialPrompt;        // style / vocabulary prompt (see text::buildSttPrompt)
        // Shrink the encoder context to the utterance length (whisper always encodes a 30 s
        // window otherwise). Roughly halves STT time for short utterances at a small accuracy
        // cost; ignored when the audio is longer than ~20 s.
        bool adaptiveAudioContext = true;
    };

    // Language of the audio by whisper's detector, run on the same reduced encoder window the
    // transcription uses. Empty when it fails. Lets the pipeline choose the per-language prompt
    // before transcribing "auto" input; whisper's own auto mode decodes without one.
    std::string detectLanguage(const float* pcm, std::size_t n, bool adaptiveAudioContext = true);

    // lang: ISO-639-1 or "auto". Audio shorter than ~1 s is zero-padded (whisper minimum).
    SttResult transcribe(const float* pcm, std::size_t n, const std::string& lang, const DecodeOptions& opts);
    SttResult transcribe(const float* pcm, std::size_t n, const std::string& lang) {
        return transcribe(pcm, n, lang, DecodeOptions());
    }

private:
    struct Impl;
    Impl* impl_;
};

} // namespace translator
