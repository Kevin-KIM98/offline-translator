#include "translator/Denoiser.hpp"
#include "translator/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if TRANSLATOR_HAS_RNNOISE
#include "rnnoise.h"
#endif

namespace translator {

struct Denoiser::Impl {
#if TRANSLATOR_HAS_RNNOISE
    DenoiseState* st = nullptr;
#endif
    // Energy-gate fallback state (also used to smooth RNNoise VAD).
    float noiseFloor = 1e-4f;
    float smoothedVad = 0.0f;
    int framesSeen = 0;
};

Denoiser::Denoiser() : impl_(new Impl) {}

Denoiser::~Denoiser() {
#if TRANSLATOR_HAS_RNNOISE
    if (impl_->st) rnnoise_destroy(impl_->st);
#endif
    delete impl_;
}

bool Denoiser::isNativeRnnoise() const {
#if TRANSLATOR_HAS_RNNOISE
    return impl_->st != nullptr;
#else
    return false;
#endif
}

bool Denoiser::init() {
#if TRANSLATOR_HAS_RNNOISE
    if (!impl_->st) impl_->st = rnnoise_create(nullptr);
    ready_ = impl_->st != nullptr;
#else
    ready_ = true;
#endif
    return ready_;
}

void Denoiser::reset() {
#if TRANSLATOR_HAS_RNNOISE
    if (impl_->st) {
        rnnoise_destroy(impl_->st);
        impl_->st = rnnoise_create(nullptr);
    }
#endif
    impl_->noiseFloor = 1e-4f;
    impl_->smoothedVad = 0.0f;
    impl_->framesSeen = 0;
}

float Denoiser::process(const float* in480, float* out480) {
    if (!ready_) {
        if (in480 != out480) std::memcpy(out480, in480, sizeof(float) * kFrameSize);
        return 0.0f;
    }

#if TRANSLATOR_HAS_RNNOISE
    if (impl_->st) {
        // RNNoise expects 16-bit PCM range (-32768..32767) as floats, not normalized audio.
        float scaled[kFrameSize];
        for (int i = 0; i < kFrameSize; ++i) scaled[i] = in480[i] * 32768.0f;
        const float vad = rnnoise_process_frame(impl_->st, scaled, scaled);
        for (int i = 0; i < kFrameSize; ++i) out480[i] = std::max(-1.0f, std::min(1.0f, scaled[i] / 32768.0f));
        impl_->smoothedVad = 0.7f * impl_->smoothedVad + 0.3f * vad;
        return impl_->smoothedVad;
    }
#endif

    // Fallback: pass-through + adaptive energy gate.
    double energy = 0.0;
    for (int i = 0; i < kFrameSize; ++i) energy += double(in480[i]) * double(in480[i]);
    energy /= kFrameSize;
    const float rms = static_cast<float>(std::sqrt(energy));

    // Warm-up: let the first ~0.5 s establish the noise floor quickly, then track it
    // slowly upward and quickly downward.
    constexpr int kWarmupFrames = 16;
    if (impl_->framesSeen < kWarmupFrames) {
        ++impl_->framesSeen;
        impl_->noiseFloor = impl_->framesSeen == 1 ? rms : 0.7f * impl_->noiseFloor + 0.3f * rms;
    } else if (rms < impl_->noiseFloor) {
        impl_->noiseFloor = 0.9f * impl_->noiseFloor + 0.1f * rms;
    } else {
        impl_->noiseFloor = 0.995f * impl_->noiseFloor + 0.005f * rms;
    }
    impl_->noiseFloor = std::max(impl_->noiseFloor, 1e-5f);

    constexpr float kAbsoluteSilence = 0.002f; // ≈ -54 dBFS: never speech regardless of SNR
    float vad = 0.0f;
    if (rms >= kAbsoluteSilence && impl_->framesSeen >= kWarmupFrames) {
        const float snr = rms / impl_->noiseFloor;
        vad = std::max(0.0f, std::min(1.0f, (snr - 2.0f) / 6.0f)); // 2x → 0, 8x → 1
    }
    impl_->smoothedVad = 0.7f * impl_->smoothedVad + 0.3f * vad;

    if (in480 != out480) std::memcpy(out480, in480, sizeof(float) * kFrameSize);
    return impl_->smoothedVad;
}

} // namespace translator
