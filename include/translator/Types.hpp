// Shared types for the offline translation engine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "translator/MiniJson.hpp"

namespace translator {

constexpr int kSampleRate = 16000;   // Hz — the whole pipeline runs at 16 kHz mono
constexpr int kFrameSize = 480;      // RNNoise frame: 480 samples = 30 ms @ 16 kHz

// ISO-639-1 codes used throughout: "ko", "en", "ja", "zh", "es". "auto" = detect.
inline const std::vector<std::string>& supportedLanguages() {
    static const std::vector<std::string> langs = {"ko", "en", "ja", "zh", "es"};
    return langs;
}
inline bool isSupportedLanguage(const std::string& code) {
    for (const auto& l : supportedLanguages())
        if (l == code) return true;
    return false;
}

struct PipelineConfig {
    std::string whisperModelPath;       // <models>/stt/whisper-small-q4.bin
    std::string nmtRootDir;             // <models>/nmt
    int nThreads = 4;                   // whisper / CTranslate2 intra-op threads
    bool useGpu = true;                 // whisper: Metal (iOS) / Vulkan-OpenCL (Android) when compiled in
    bool enableDenoise = true;          // RNNoise front-end
    int beamSize = 2;                   // NMT beam (1 = greedy, fastest)
    int maxDecodingLength = 256;        // NMT hard stop
    std::vector<std::string> pivotLangs = {"ko", "en"}; // tried in order when no direct pair exists
    std::string initialPrompt;          // optional whisper prompt (domain vocabulary, punctuation style)
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
    std::vector<std::string> route;     // e.g. {"ja-ko","ko-en"} when pivoting
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
