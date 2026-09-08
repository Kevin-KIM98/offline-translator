// Turns a continuous stream of denoised 30 ms frames + VAD probabilities into discrete
// utterances suitable for whisper. Implements start/stop hysteresis, pre-roll, minimum
// and maximum utterance lengths.
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace translator {

struct SegmenterConfig {
    float startThreshold = 0.60f;  // VAD prob to begin an utterance
    float endThreshold = 0.35f;    // VAD prob under which a frame counts as silence
    int startFrames = 3;           // consecutive voiced frames needed to start (3 × 30 ms)
    int endSilenceMs = 700;        // trailing silence that closes an utterance
    int minUtteranceMs = 400;      // shorter utterances are discarded
    int maxUtteranceMs = 15000;    // force-close (whisper works on ≤ 30 s windows)
    int preRollMs = 300;           // audio kept before the detected start
};

class SpeechSegmenter {
public:
    explicit SpeechSegmenter(const SegmenterConfig& cfg = {});

    // Push one 480-sample frame with its VAD probability.
    // Returns true if a complete utterance became available.
    bool pushFrame(const float* frame480, float vadProb);

    bool hasUtterance() const { return !ready_.empty(); }
    std::vector<float> popUtterance();
    std::size_t pendingCount() const { return ready_.size(); }

    // Close the current utterance (if any) regardless of silence, e.g. when the user
    // releases a push-to-talk button. Returns true if an utterance was emitted.
    bool flush();

    void reset();
    bool inSpeech() const { return inSpeech_; }
    const SegmenterConfig& config() const { return cfg_; }

private:
    void emitCurrent();

    SegmenterConfig cfg_;
    std::deque<std::vector<float>> preRoll_;
    std::vector<float> current_;
    std::deque<std::vector<float>> ready_;
    bool inSpeech_ = false;
    int voicedRun_ = 0;
    int silenceRun_ = 0;
    int preRollFrames_ = 0;
    int endSilenceFrames_ = 0;
    std::size_t minSamples_ = 0;
    std::size_t maxSamples_ = 0;
};

} // namespace translator
