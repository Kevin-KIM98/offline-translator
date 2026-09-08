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

    // lang: ISO-639-1 or "auto". Audio shorter than ~1 s is zero-padded (whisper minimum).
    SttResult transcribe(const float* pcm, std::size_t n, const std::string& lang, const std::string& initialPrompt = "");

private:
    struct Impl;
    Impl* impl_;
};

} // namespace translator
