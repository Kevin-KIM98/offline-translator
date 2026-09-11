// Shared types for the offline translation engine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "translator/MiniJson.hpp"

namespace translator {

constexpr int kSampleRate = 16000;   // Hz — the whole pipeline runs at 16 kHz mono
constexpr int kFrameSize = 480;      // RNNoise frame: 480 samples = 30 ms @ 16 kHz

// ISO-639-1 codes used throughout: ko en es vi th ja zh. "auto" = detect.
inline const std::vector<std::string>& supportedLanguages() {
    static const std::vector<std::string> langs = {"ko", "en", "es", "vi", "th", "ja", "zh"};
    return langs;
}

// Which translation engine handles a request.
enum class TranslationBackend {
    Auto,    // Marian pair if one exists (direct or via pivot), otherwise the LLM
    Marian,  // CTranslate2 OPUS-MT only
    Llm,     // LLM only (any→any in one hop; source may be "auto")
};

// Demonstrations placed in the LLM prompt ahead of the text to translate.
enum class LlmExamples {
    None,     // instruction only
    Single,   // one "where is the nearest station?" pair (up to 0.3.6); leaks into similar questions
    Diverse,  // three unrelated sentences: a statement, a request and a count (default)
};
inline bool isSupportedLanguage(const std::string& code) {
    for (const auto& l : supportedLanguages())
        if (l == code) return true;
    return false;
}

struct PipelineConfig {
    std::string whisperModelPath;       // <models>/stt/whisper-small-q5_1.bin
    std::string nmtRootDir;             // <models>/nmt
    std::string llmModelPath;           // <models>/llm/<model>.gguf (optional)
    TranslationBackend backend = TranslationBackend::Auto;
    int llmContextSize = 1024;
    int llmMaxOutputTokens = 256;
    float llmTemperature = 0.0f;        // 0 = greedy
    int llmGpuLayers = 99;              // Metal / Vulkan offload when compiled in
    std::string llmSystemPrompt;        // override the built-in translation instruction
    LlmExamples llmExamples = LlmExamples::Diverse;  // demonstrations in the LLM prompt
    // Auto backend, direction with no Marian route: translate to English with Marian first and give
    // the LLM English, which it handles far better than Korean or Japanese.
    bool llmPivotThroughEnglish = true;
    int nThreads = 4;                   // whisper / CTranslate2 intra-op threads
    bool useGpu = true;                 // whisper: Metal (iOS) / Vulkan-OpenCL (Android) when compiled in
    bool enableDenoise = true;          // RNNoise front-end: voice activity and the level the loudness gate uses
    // Give whisper RNNoise's output instead of the microphone audio. Off by default: whisper is
    // trained on noisy speech, and on clean speech the denoiser's artifacts cost words.
    bool sttOnDenoisedAudio = false;
    int beamSize = 4;                   // NMT beam (1 = greedy, fastest)
    int maxDecodingLength = 256;        // NMT hard stop
    int noRepeatNgramSize = 3;          // NMT: block repeated n-grams (0 = off)
    float repetitionPenalty = 1.1f;     // NMT: >1 discourages repeating tokens
    // Tried in order when no direct pair exists. X→en / en→X OPUS-MT models are the strongest,
    // so English is the preferred pivot.
    std::vector<std::string> pivotLangs = {"en", "ko"};
    std::string initialPrompt;          // optional whisper prompt (domain vocabulary, punctuation style)
    bool useDefaultPrompts = true;      // when initialPrompt is empty use a punctuated per-language prompt
    bool useContextPrompt = true;       // append the previous utterance's transcript to the prompt
    int sttBeamSize = 5;                // whisper beam search width (1 = greedy)
    float noSpeechThreshold = 0.85f;    // drop whisper segments whose no-speech probability exceeds this
    bool sttAdaptiveAudioContext = true;// shrink whisper's encoder window to the utterance length (faster)
    bool cleanTranscripts = true;       // filter hallucinations / repetitions from STT output
    bool postProcessTranslations = true;// spacing / capitalization fixes on NMT output
    bool preloadAllPairs = false;       // load every NMT pair found on disk at init (memory heavy)
};

struct SttResult {
    bool ok = false;
    std::string error;
    std::string text;
    std::string detectedLang;           // ISO-639-1 as reported by whisper
    double elapsedMs = 0.0;
};

struct TranslationResult {
    bool ok = false;
    std::string error;
    std::string sourceText;
    std::string sourceLang;
    std::string targetLang;
    std::string translatedText;
    std::vector<std::string> route;     // e.g. {"ja-ko","ko-en"} when pivoting, {"llm"} for the LLM
    double sttMs = 0.0;
    double nmtMs = 0.0;
    double totalMs = 0.0;

    Json toJson() const {
        Json j = Json::object();
        j.set("ok", ok);
        if (!error.empty()) j.set("error", error);
        j.set("source_text", sourceText);
        j.set("source_lang", sourceLang);
        j.set("target_lang", targetLang);
        j.set("translated_text", translatedText);
        Json r = Json::array();
        for (const auto& hop : route) r.push_back(hop);
        j.set("route", r);
        Json t = Json::object();
        t.set("stt_ms", sttMs);
        t.set("nmt_ms", nmtMs);
        t.set("total_ms", totalMs);
        j.set("timings", t);
        return j;
    }
};

} // namespace translator
