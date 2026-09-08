// CTranslate2 + SentencePiece wrapper managing one translator per language pair.
//
// Directory layout expected under nmtRootDir:
//   <root>/<src>-<tgt>/model.bin              (CTranslate2 converted MarianMT / NLLB model)
//   <root>/<src>-<tgt>/shared_vocabulary.json (or .txt, written by the converter)
//   <root>/<src>-<tgt>/source.spm, target.spm (per-pair SentencePiece; optional)
//   <root>/<src>-<tgt>/pair.json              (optional: {"source_prefix_token": ">>kor<<"})
//   <root>/tokenizer/source.spm, target.spm   (shared fallback tokenizer)
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace translator {

class NmtEngine {
public:
    using Hop = std::pair<std::string, std::string>; // (src, tgt)

    NmtEngine();
    ~NmtEngine();
    NmtEngine(const NmtEngine&) = delete;
    NmtEngine& operator=(const NmtEngine&) = delete;

    struct Options {
        int nThreads = 4;
        int beamSize = 4;
        int maxDecodingLength = 256;
        int noRepeatNgramSize = 3;     // 0 = off
        float repetitionPenalty = 1.1f;
    };
    void init(const std::string& rootDir, const Options& options);
    void init(const std::string& rootDir, int nThreads, int beamSize, int maxDecodingLength) {
        Options o;
        o.nThreads = nThreads;
        o.beamSize = beamSize;
        o.maxDecodingLength = maxDecodingLength;
        init(rootDir, o);
    }
    const std::string& rootDir() const;

    static std::string pairName(const std::string& src, const std::string& tgt) { return src + "-" + tgt; }

    // Pairs present on disk (directories named "xx-yy" containing model.bin).
    std::vector<std::string> availablePairs() const;
    bool hasPair(const std::string& src, const std::string& tgt) const;
    bool isLoaded(const std::string& src, const std::string& tgt) const;

    bool loadPair(const std::string& src, const std::string& tgt, std::string* error = nullptr);
    void unloadPair(const std::string& src, const std::string& tgt);
    void unloadAll();

    // Direct pair if present, otherwise src→pivot→tgt for the first pivot that works.
    // Returns an empty vector when src == tgt or no route exists (check `error`).
    std::vector<Hop> resolveRoute(const std::string& src, const std::string& tgt,
                                  const std::vector<std::string>& pivots, std::string* error = nullptr) const;

    // Full translation with automatic routing and lazy pair loading.
    bool translate(const std::string& text, const std::string& src, const std::string& tgt,
                   const std::vector<std::string>& pivots, std::string& out,
                   std::vector<std::string>* routeOut = nullptr, std::string* error = nullptr);

    // Single hop; pair must already be loaded (or loadable).
    bool translateDirect(const std::string& text, const std::string& src, const std::string& tgt,
                         std::string& out, std::string* error = nullptr);

    bool isNativeCTranslate2() const;

    // Splits text into sentences on ., !, ?, 。, ！, ？ and newlines (exposed for tests).
    static std::vector<std::string> splitSentences(const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace translator
