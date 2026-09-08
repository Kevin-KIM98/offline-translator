#include "translator/SpeechSegmenter.hpp"
#include "translator/Types.hpp"

#include <algorithm>

namespace translator {

namespace {
constexpr int kFrameMs = kFrameSize * 1000 / kSampleRate; // 30
int msToFrames(int ms) { return std::max(1, (ms + kFrameMs - 1) / kFrameMs); }
} // namespace

SpeechSegmenter::SpeechSegmenter(const SegmenterConfig& cfg) : cfg_(cfg) {
    preRollFrames_ = msToFrames(cfg_.preRollMs);
    endSilenceFrames_ = msToFrames(cfg_.endSilenceMs);
    minSamples_ = static_cast<std::size_t>(cfg_.minUtteranceMs) * kSampleRate / 1000;
    maxSamples_ = static_cast<std::size_t>(cfg_.maxUtteranceMs) * kSampleRate / 1000;
}

void SpeechSegmenter::reset() {
    preRoll_.clear();
    current_.clear();
    ready_.clear();
    inSpeech_ = false;
    voicedRun_ = 0;
    silenceRun_ = 0;
}

void SpeechSegmenter::emitCurrent() {
    if (current_.size() >= minSamples_) ready_.push_back(std::move(current_));
    current_.clear();
    inSpeech_ = false;
    voicedRun_ = 0;
    silenceRun_ = 0;
}

bool SpeechSegmenter::pushFrame(const float* frame480, float vadProb) {
    const std::size_t before = ready_.size();

    if (!inSpeech_) {
        preRoll_.emplace_back(frame480, frame480 + kFrameSize);
        while (static_cast<int>(preRoll_.size()) > preRollFrames_) preRoll_.pop_front();

        if (vadProb >= cfg_.startThreshold) {
            if (++voicedRun_ >= cfg_.startFrames) {
                inSpeech_ = true;
                silenceRun_ = 0;
                current_.clear();
                for (const auto& f : preRoll_) current_.insert(current_.end(), f.begin(), f.end());
                preRoll_.clear();
            }
        } else {
            voicedRun_ = 0;
        }
        return false;
    }

    current_.insert(current_.end(), frame480, frame480 + kFrameSize);

    if (vadProb < cfg_.endThreshold) {
        if (++silenceRun_ >= endSilenceFrames_) {
            // Trim most of the trailing silence, keep ~150 ms so whisper sees a clean end.
            const std::size_t keep = static_cast<std::size_t>(150) * kSampleRate / 1000;
            const std::size_t silence = static_cast<std::size_t>(silenceRun_) * kFrameSize;
            if (silence > keep && current_.size() > silence - keep) current_.resize(current_.size() - (silence - keep));
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
