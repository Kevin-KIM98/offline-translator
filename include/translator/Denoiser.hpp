// RNNoise front-end. Consumes/produces 480-sample float frames in [-1, 1] at 16 kHz
// and reports a per-frame voice-activity probability.
//
// When the library is built without RNNoise (TRANSLATOR_HAS_RNNOISE=0) the denoiser is a
// pass-through and VAD falls back to a simple energy gate, so the rest of the pipeline
// still works on desktop test builds.
#pragma once

#include <cstddef>

namespace translator {

class Denoiser {
public:
    Denoiser();
    ~Denoiser();
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    bool init();
    bool isReady() const { return ready_; }
    bool isNativeRnnoise() const;

    // in/out may alias. Returns VAD probability in [0, 1].
    float process(const float* in480, float* out480);

    // Voice activity from frame energy over a tracked floor, without touching the audio. Used
    // by process() when RNNoise is absent, and by the pipeline when the denoiser is switched
    // off, so utterances still close on pauses instead of waiting for a flush.
    float energyVad(const float* in480);

    void reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

} // namespace translator
