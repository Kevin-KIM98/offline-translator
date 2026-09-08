// llama.cpp-based translation backend: one small multilingual instruction-tuned LLM (GGUF,
// e.g. Qwen2.5-1.5B-Instruct Q4_K_M) translates between ANY pair of supported languages in a
// single hop, with automatic source-language handling. Complements the per-pair Marian models:
// the pipeline uses Marian where a good direct model exists (fastest) and the LLM for everything
// else (Thai, Vietnamese, ko↔ja, ...), or LLM-only when configured.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace translator {

struct LlmOptions {
    int nThreads = 4;
    int contextSize = 1024;      // prompt + output tokens
    int maxOutputTokens = 256;   // hard cap per translation (also bounded by input length)
    float temperature = 0.0f;    // 0 = greedy (recommended for translation)
    int gpuLayers = 99;          // offload everything when a GPU backend (Metal/Vulkan) is compiled in
    bool useMmap = true;
    std::string systemPrompt;    // override the built-in translation instruction (advanced)
};

struct LlmResult {
    bool ok = false;
    std::string error;
    std::string text;
    int promptTokens = 0;
    int outputTokens = 0;
    double elapsedMs = 0.0;
};

class LlmEngine {
public:
    LlmEngine();
    ~LlmEngine();
    LlmEngine(const LlmEngine&) = delete;
    LlmEngine& operator=(const LlmEngine&) = delete;

    bool load(const std::string& ggufPath, const LlmOptions& opts, std::string* error = nullptr);
    void unload();
    bool isLoaded() const;
    static bool isNativeLlama();

    // src may be "auto" (the model is told to detect the language itself).
    // Language codes: ko en es vi th ja zh (ISO-639-1); anything else is passed as-is.
    LlmResult translate(const std::string& text, const std::string& src, const std::string& tgt);

    // Human-readable language name used in prompts ("Korean", "Vietnamese", ...).
    static std::string languageName(const std::string& code);
    // The prompt the engine sends (exposed for tests / debugging).
    static std::string buildInstruction(const std::string& src, const std::string& tgt, const std::string& systemPrompt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace translator
