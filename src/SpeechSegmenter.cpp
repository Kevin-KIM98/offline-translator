#include "translator/SpeechSegmenter.hpp"
#include "translator/Types.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace translator {

namespace {
constexpr int kFrameMs = kFrameSize * 1000 / kSampleRate; // 30
int msToFrames(int ms) { return std::max(1, (ms + kFrameMs - 1) / kFrameMs); }

float frameRms(const float* frame) {
    double sum = 0.0;
    for (int i = 0; i < kFrameSize; ++i) sum += static_cast<double>(frame[i]) * frame[i];
    return static_cast<float>(std::sqrt(sum / kFrameSize));
}

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
} // namespace

SpeechSegmenter::SpeechSegmenter(const SegmenterConfig& cfg) : cfg_(cfg) {
    preRollFrames_ = msToFrames(cfg_.preRollMs);
    endSilenceFrames_ = msToFrames(cfg_.endSilenceMs);
    minSamples_ = static_cast<std::size_t>(cfg_.minUtteranceMs) * kSampleRate / 1000;
    maxSamples_ = static_cast<std::size_t>(cfg_.maxUtteranceMs) * kSampleRate / 1000;
}

void SpeechSegmenter::reset() {
    preRoll_.clear();
    preRollRms_.clear();
    current_.clear();
    currentRms_.clear();
    ready_.clear();
    inSpeech_ = false;
    voicedRun_ = 0;
    silenceRun_ = 0;
    noiseFloor_ = -1.0f;
}

float SpeechSegmenter::noiseFloorDbfs() const {
    if (noiseFloor_ < 0.0f) return -std::numeric_limits<float>::infinity();
    return 20.0f * std::log10(std::max(noiseFloor_, 1e-7f));
}

void SpeechSegmenter::appendFrame(const float* frame480, float rms) {
    current_.insert(current_.end(), frame480, frame480 + kFrameSize);
    currentRms_.push_back(rms);
}

void SpeechSegmenter::trimTail(std::size_t samples) {
    if (samples == 0 || samples > current_.size()) return;
    current_.resize(current_.size() - samples);
    const std::size_t frames = samples / kFrameSize;
    if (frames >= currentRms_.size()) currentRms_.clear();
    else currentRms_.resize(currentRms_.size() - frames);
}

/// True when the utterance is loud enough to be speech rather than room noise.
///
/// The median frame is used, not the peak: a keyboard tap or a door lifts the peak of an
/// otherwise silent segment, while speech raises the whole distribution.
bool SpeechSegmenter::loudEnough() const {
    if (currentRms_.empty()) return false;
    std::vector<float> sorted(currentRms_);
    const std::size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(mid), sorted.end());
    const float median = sorted[mid];

    float threshold = dbToLinear(cfg_.minSpeechDbfs);
    if (noiseFloor_ > 0.0f) threshold = std::max(threshold, noiseFloor_ * dbToLinear(cfg_.speechAboveNoiseDb));
    return median >= threshold;
}

void SpeechSegmenter::emitCurrent() {
    if (current_.size() >= minSamples_) {
        if (loudEnough()) ready_.push_back(std::move(current_));
        else ++droppedQuiet_;
    }
    current_.clear();
    currentRms_.clear();
    inSpeech_ = false;
    voicedRun_ = 0;
    silenceRun_ = 0;
}

bool SpeechSegmenter::pushFrame(const float* frame480, float vadProb) {
    const std::size_t before = ready_.size();
    const float rms = frameRms(frame480);

    if (!inSpeech_) {
        // Track the room while nobody is talking. Falls quickly so a quieter room is picked up
        // within a second, creeps up slowly so a passing noise does not raise the bar for speech.
        if (vadProb < cfg_.endThreshold) {
            if (noiseFloor_ < 0.0f) noiseFloor_ = rms;
            else if (rms < noiseFloor_) noiseFloor_ = 0.7f * noiseFloor_ + 0.3f * rms;
            else noiseFloor_ = 0.99f * noiseFloor_ + 0.01f * rms;
        }

        preRoll_.emplace_back(frame480, frame480 + kFrameSize);
        preRollRms_.push_back(rms);
        while (static_cast<int>(preRoll_.size()) > preRollFrames_) {
            preRoll_.pop_front();
            preRollRms_.pop_front();
        }

        if (vadProb >= cfg_.startThreshold) {
            if (++voicedRun_ >= cfg_.startFrames) {
                inSpeech_ = true;
                silenceRun_ = 0;
                current_.clear();
                currentRms_.clear();
                for (std::size_t i = 0; i < preRoll_.size(); ++i) {
                    current_.insert(current_.end(), preRoll_[i].begin(), preRoll_[i].end());
                    currentRms_.push_back(preRollRms_[i]);
                }
                preRoll_.clear();
                preRollRms_.clear();
            }
        } else {
            voicedRun_ = 0;
        }
        return false;
    }

    appendFrame(frame480, rms);

    if (vadProb < cfg_.endThreshold) {
        if (++silenceRun_ >= endSilenceFrames_) {
            // Trim most of the trailing silence, keep ~150 ms so whisper sees a clean end.
            const std::size_t keep = static_cast<std::size_t>(150) * kSampleRate / 1000;
            const std::size_t silence = static_cast<std::size_t>(silenceRun_) * kFrameSize;
            if (silence > keep && current_.size() > silence - keep) trimTail(silence - keep);
            emitCurrent();
        }
    } else {
        silenceRun_ = 0;
    }

    if (inSpeech_ && current_.size() >= maxSamples_) emitCurrent();

    return ready_.size() > before;
}

bool SpeechSegmenter::flush() {
    if (!inSpeech_) return false;
    const std::size_t before = ready_.size();
    emitCurrent();
    return ready_.size() > before;
}

std::vector<float> SpeechSegmenter::popUtterance() {
    if (ready_.empty()) return {};
    std::vector<float> out = std::move(ready_.front());
    ready_.pop_front();
    return out;
}

} // namespace translator
