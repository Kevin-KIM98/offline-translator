// Turns a continuous stream of 30 ms frames + VAD probabilities into discrete utterances
// suitable for whisper. Implements start/stop hysteresis, pre-roll, minimum and maximum
// utterance lengths, and a loudness gate against room noise.
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

    // Loudness gate. RNNoise's voice-activity probability alone fires on room noise, and
    // whisper answers a noise-only segment with a fluent invented sentence. An utterance is
    // therefore kept only when its median frame sits this far above the tracked noise floor.
    // Measured on a quiet desk microphone: noise windows reach 6.7 dB above the floor, speech
    // stays at least 18.9 dB above it.
    float speechAboveNoiseDb = 12.0f;
    // Absolute backstop, so a digitally silent input cannot pass the relative test.
    float minSpeechDbfs = -60.0f;
};

class SpeechSegmenter {
public:
    explicit SpeechSegmenter(const SegmenterConfig& cfg = {});

    // Push one 480-sample frame with its VAD probability. `frame480` is what the utterance is
    // built from; the loudness gate measures `levelFrame480` when given (the pipeline passes the
    // denoised frame there and the microphone frame as the audio, so the thresholds keep the
    // meaning they were measured with), else the audio frame itself.
    // Returns true if a complete utterance became available.
    bool pushFrame(const float* frame480, float vadProb, const float* levelFrame480 = nullptr);

    bool hasUtterance() const { return !ready_.empty(); }
    std::vector<float> popUtterance();
    std::size_t pendingCount() const { return ready_.size(); }

    // Close the current utterance (if any) regardless of silence, e.g. when the user
    // releases a push-to-talk button. Returns true if an utterance was emitted.
    bool flush();

    void reset();
    bool inSpeech() const { return inSpeech_; }
    const SegmenterConfig& config() const { return cfg_; }

    // Utterances discarded by the loudness gate, and the current noise floor in dBFS
    // (-inf before the first frame). Both are diagnostics.
    std::size_t droppedQuietCount() const { return droppedQuiet_; }
    float noiseFloorDbfs() const;

private:
    void emitCurrent();
    void appendFrame(const float* frame480, float rms);
    void trimTail(std::size_t samples);
    bool loudEnough() const;

    SegmenterConfig cfg_;
    std::deque<std::vector<float>> preRoll_;
    std::deque<float> preRollRms_;
    std::vector<float> current_;
    std::vector<float> currentRms_;   // one entry per frame in current_
    std::deque<std::vector<float>> ready_;
    bool inSpeech_ = false;
    int voicedRun_ = 0;
    int silenceRun_ = 0;
    int preRollFrames_ = 0;
    int endSilenceFrames_ = 0;
    std::size_t minSamples_ = 0;
    std::size_t maxSamples_ = 0;
    float noiseFloor_ = -1.0f;        // linear RMS; negative until the first frame
    std::size_t droppedQuiet_ = 0;
};

} // namespace translator
