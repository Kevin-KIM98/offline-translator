// Language-aware text helpers used around STT and NMT:
//   * default whisper prompts (punctuated, per language) — whisper copies the style of the
//     prompt, so a punctuated prompt yields punctuated output, which in turn lets the NMT
//     stage translate sentence by sentence.
//   * transcript cleaning — drops whisper hallucinations ("시청해주셔서 감사합니다", "[음악]",
//     "Thank you for watching.") and runaway repetitions.
//   * translation post-processing — capitalization / spacing rules per target language.
#pragma once

#include <string>
#include <vector>

namespace translator {
namespace text {

// Punctuated example sentences in `lang` (empty for unknown languages).
std::string defaultPromptFor(const std::string& lang);

// Builds the prompt passed to whisper: user prompt (if any) or the language default, followed
// by the tail of the previous transcript so consecutive utterances share vocabulary/style.
// `maxContextChars` bounds the context tail (whisper keeps ~224 tokens of prompt).
std::string buildSttPrompt(const std::string& userPrompt, const std::string& lang,
                           const std::string& previousTranscript, std::size_t maxContextChars = 200);

// Returns true if `s` is a known whisper hallucination for `lang` (or language-agnostic).
bool isHallucination(const std::string& s, const std::string& lang);

// Removes bracketed non-speech markers, music notes, hallucinated phrases and runaway
// repetitions. Returns an empty string when nothing meaningful remains.
std::string cleanTranscript(const std::string& s, const std::string& lang);

// Fixes spacing/capitalization of MT output for the target language.
std::string postProcessTranslation(const std::string& s, const std::string& lang);

// UTF-8 helpers (exposed for tests).
std::vector<std::string> utf8Chars(const std::string& s);
bool isCjkChar(const std::string& ch);
std::string trim(const std::string& s);
std::string collapseSpaces(const std::string& s);

} // namespace text
} // namespace translator
