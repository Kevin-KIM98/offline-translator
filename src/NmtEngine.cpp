#include "translator/NmtEngine.hpp"

#include "translator/FileUtil.hpp"
#include "translator/MiniJson.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#if TRANSLATOR_HAS_CTRANSLATE2
#include <ctranslate2/translator.h>
#endif
#if TRANSLATOR_HAS_SENTENCEPIECE
#include <sentencepiece_processor.h>
#endif

namespace translator {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool isCjk(const std::string& lang) { return lang == "ja" || lang == "zh"; }

bool looksLikePairDir(const std::string& name) {
    // "xx-yy" or "xxx-yyy" (ISO codes only, lowercase)
    const auto dash = name.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= name.size()) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
    return name.find('-', dash + 1) == std::string::npos;
}

} // namespace

struct LoadedPair {
#if TRANSLATOR_HAS_CTRANSLATE2
    std::unique_ptr<ctranslate2::Translator> translator;
#endif
#if TRANSLATOR_HAS_SENTENCEPIECE
    std::unique_ptr<sentencepiece::SentencePieceProcessor> spSource;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> spTarget;
#endif
    std::string sourcePrefixToken; // e.g. ">>kor<<" for multilingual OPUS-MT targets
    std::string targetPrefixToken; // e.g. "kor_Hang" for NLLB-style decoders
    std::string sourceBosToken;    // prepended when config.json add_source_bos=false but a BOS is expected
    std::string sourceEosToken;    // "</s>" for transformers-converted Marian models (add_source_eos=false)
    std::string dir;
};

struct NmtEngine::Impl {
    std::string rootDir;
    int nThreads = 4;
    int beamSize = 4;
    int maxDecodingLength = 256;
    int noRepeatNgramSize = 3;
    float repetitionPenalty = 1.1f;
    std::map<std::string, std::unique_ptr<LoadedPair>> loaded; // key: "src-tgt"
    mutable std::mutex mutex;

    std::string pairDir(const std::string& src, const std::string& tgt) const {
        return fs::join(rootDir, pairName(src, tgt));
    }

    std::string findTokenizer(const std::string& pairDir, const char* name) const {
        const std::string local = fs::join(pairDir, name);
        if (fs::isFile(local)) return local;
        const std::string shared = fs::join(fs::join(rootDir, "tokenizer"), name);
        if (fs::isFile(shared)) return shared;
        return {};
    }

    // Tokenize one sentence into subword pieces.
    std::vector<std::string> encode(LoadedPair& p, const std::string& sentence) const {
        std::vector<std::string> pieces;
#if TRANSLATOR_HAS_SENTENCEPIECE
        if (p.spSource) {
            p.spSource->Encode(sentence, &pieces);
        } else
#endif
        {
            // Whitespace fallback (test builds only — real models need SentencePiece).
            std::istringstream iss(sentence);
            std::string tok;
            while (iss >> tok) pieces.push_back(tok);
        }
        if (!p.sourcePrefixToken.empty()) pieces.insert(pieces.begin(), p.sourcePrefixToken);
        if (!p.sourceBosToken.empty()) pieces.insert(pieces.begin(), p.sourceBosToken);
        if (!p.sourceEosToken.empty()) pieces.push_back(p.sourceEosToken);
        return pieces;
    }

    std::string decode(LoadedPair& p, const std::vector<std::string>& pieces) const {
        std::vector<std::string> clean;
        clean.reserve(pieces.size());
        for (const auto& t : pieces) {
            if (t == "</s>" || t == "<s>" || t == "<pad>" || t == "<unk>") continue;
            if (!p.targetPrefixToken.empty() && t == p.targetPrefixToken) continue;
            clean.push_back(t);
        }
#if TRANSLATOR_HAS_SENTENCEPIECE
        if (p.spTarget) {
            std::string text;
            p.spTarget->Decode(clean, &text);
            return text;
        }
#endif
        std::string text;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            std::string t = clean[i];
            const bool wordStart = t.rfind("\xE2\x96\x81", 0) == 0; // "▁"
            if (wordStart) t = t.substr(3);
            if (i && (wordStart || !clean[i].empty())) text.push_back(' ');
            text += t;
        }
        return text;
    }
};

NmtEngine::NmtEngine() : impl_(new Impl) {}
NmtEngine::~NmtEngine() = default;

void NmtEngine::init(const std::string& rootDir, const Options& o) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->loaded.clear();
    impl_->rootDir = rootDir;
    impl_->nThreads = o.nThreads > 0 ? o.nThreads : 4;
    impl_->beamSize = o.beamSize > 0 ? o.beamSize : 1;
    impl_->maxDecodingLength = o.maxDecodingLength > 0 ? o.maxDecodingLength : 256;
    impl_->noRepeatNgramSize = o.noRepeatNgramSize > 0 ? o.noRepeatNgramSize : 0;
    impl_->repetitionPenalty = o.repetitionPenalty > 0.0f ? o.repetitionPenalty : 1.0f;
}

const std::string& NmtEngine::rootDir() const { return impl_->rootDir; }

bool NmtEngine::isNativeCTranslate2() const {
#if TRANSLATOR_HAS_CTRANSLATE2
    return true;
#else
    return false;
#endif
}

std::vector<std::string> NmtEngine::availablePairs() const {
    std::vector<std::string> out;
    for (const auto& name : fs::listSubdirectories(impl_->rootDir)) {
        if (!looksLikePairDir(name)) continue;
        if (fs::isFile(fs::join(fs::join(impl_->rootDir, name), "model.bin"))) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool NmtEngine::hasPair(const std::string& src, const std::string& tgt) const {
    return fs::isFile(fs::join(impl_->pairDir(src, tgt), "model.bin"));
}

bool NmtEngine::isLoaded(const std::string& src, const std::string& tgt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->loaded.count(pairName(src, tgt)) > 0;
}

bool NmtEngine::loadPair(const std::string& src, const std::string& tgt, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key = pairName(src, tgt);
    if (impl_->loaded.count(key)) return true;

    const std::string dir = impl_->pairDir(src, tgt);
    if (!fs::isFile(fs::join(dir, "model.bin"))) {
        if (error) *error = "NMT model not found: " + dir;
        return false;
    }

    auto p = std::make_unique<LoadedPair>();
    p->dir = dir;

    // CTranslate2's config.json says whether the runtime adds BOS/EOS to the source itself.
    // Models converted from transformers (OPUS-MT / Marian) have add_source_eos=false and
    // expect the tokenizer to append "</s>", which we do here.
    std::string cfgText;
    if (fs::readFile(fs::join(dir, "config.json"), cfgText)) {
        const Json cfg = Json::parse(cfgText);
        if (cfg.isObject()) {
            if (!cfg.getBool("add_source_eos", false)) p->sourceEosToken = cfg.getString("eos_token", "</s>");
            // BOS is only expected by a few specs (e.g. NLLB/M2M); opt-in via pair.json below.
        }
    } else {
        p->sourceEosToken = "</s>";
    }

    // Optional pair.json overrides: prefix tokens and BOS/EOS behaviour.
    if (fs::readFile(fs::join(dir, "pair.json"), cfgText)) {
        const Json cfg = Json::parse(cfgText);
        p->sourcePrefixToken = cfg.getString("source_prefix_token");
        p->targetPrefixToken = cfg.getString("target_prefix_token");
        if (cfg.contains("source_bos_token")) p->sourceBosToken = cfg.getString("source_bos_token");
        if (cfg.contains("source_eos_token")) p->sourceEosToken = cfg.getString("source_eos_token");
    }

#if TRANSLATOR_HAS_SENTENCEPIECE
    const std::string srcSpm = impl_->findTokenizer(dir, "source.spm");
    std::string tgtSpm = impl_->findTokenizer(dir, "target.spm");
    if (tgtSpm.empty()) tgtSpm = srcSpm;
    if (srcSpm.empty()) {
        if (error) *error = "SentencePiece model (source.spm) not found for " + key;
        return false;
    }
    p->spSource = std::make_unique<sentencepiece::SentencePieceProcessor>();
    if (!p->spSource->Load(srcSpm).ok()) {
        if (error) *error = "failed to load " + srcSpm;
        return false;
    }
    if (tgtSpm == srcSpm) {
        p->spTarget = std::make_unique<sentencepiece::SentencePieceProcessor>();
        p->spTarget->Load(srcSpm);
    } else {
        p->spTarget = std::make_unique<sentencepiece::SentencePieceProcessor>();
        if (!p->spTarget->Load(tgtSpm).ok()) {
            if (error) *error = "failed to load " + tgtSpm;
            return false;
        }
    }
#endif

#if TRANSLATOR_HAS_CTRANSLATE2
    try {
        ctranslate2::ReplicaPoolConfig pool;
#if defined(_WIN32)
        // Windows only: CTranslate2 keeps a thread_local ruy::Context in its replica worker;
        // when that worker exits, the context's destructor joins ruy's threads while the
        // loader lock is held → deadlock on Translator destruction with intra threads > 1
        // (observed with MSVC 2022 + CTranslate2 4.5). INT8 Marian decoding is ~50 ms per
        // sentence single-threaded, so cap to 1 on the desktop test platform.
        pool.num_threads_per_replica = 1;
#else
        pool.num_threads_per_replica = static_cast<std::size_t>(impl_->nThreads);
#endif
        p->translator = std::make_unique<ctranslate2::Translator>(
            dir, ctranslate2::Device::CPU, ctranslate2::ComputeType::DEFAULT, std::vector<int>{0}, false, pool);
    } catch (const std::exception& e) {
        if (error) *error = std::string("CTranslate2 load failed: ") + e.what();
        return false;
    }
#endif

    impl_->loaded[key] = std::move(p);
    if (error) error->clear();
    return true;
}

void NmtEngine::unloadPair(const std::string& src, const std::string& tgt) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->loaded.erase(pairName(src, tgt));
}

void NmtEngine::unloadAll() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->loaded.clear();
}

std::vector<NmtEngine::Hop> NmtEngine::resolveRoute(const std::string& src, const std::string& tgt,
                                                    const std::vector<std::string>& pivots, std::string* error) const {
    if (src == tgt) return {};
    if (hasPair(src, tgt)) return {{src, tgt}};
    for (const auto& pivot : pivots) {
        if (pivot == src || pivot == tgt) continue;
        if (hasPair(src, pivot) && hasPair(pivot, tgt)) return {{src, pivot}, {pivot, tgt}};
    }
    if (error) *error = "no NMT route from " + src + " to " + tgt + " (need " + pairName(src, tgt) + " or a pivot pair)";
    return {};
}

std::vector<std::string> NmtEngine::splitSentences(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > text.size()) len = text.size() - i;
        const std::string ch = text.substr(i, len);
        i += len;

        if (ch == "\n") {
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
            continue;
        }
        cur += ch;
        const bool terminal = ch == "." || ch == "!" || ch == "?" ||
                              ch == "\xE3\x80\x82" /* 。 */ || ch == "\xEF\xBC\x81" /* ！ */ || ch == "\xEF\xBC\x9F" /* ？ */;
        if (terminal) {
            if (ch == ".") {
                // Don't split on decimals ("3.14"), on a period glued to the next character
                // ("p.m.", "e.g.", "a.b.c"), or after a one-letter token / known abbreviation.
                if (i < text.size()) {
                    const char nx = text[i];
                    if ((nx >= '0' && nx <= '9') || ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z'))) continue;
                }
                const std::string t = trim(cur);
                const auto sp = t.find_last_of(' ');
                std::string word = sp == std::string::npos ? t : t.substr(sp + 1);
                if (!word.empty()) word.pop_back(); // drop the period
                std::string lw;
                for (const char wc : word) lw.push_back(static_cast<char>((wc >= 'A' && wc <= 'Z') ? wc - 'A' + 'a' : wc));
                static const char* const kAbbrev[] = {"e.g", "i.e", "etc", "mr", "mrs", "ms", "dr", "prof", "vs", "st", "no", "sr", "jr", "approx", "dept", "inc", "ltd", "p.m", "a.m"};
                bool abbrev = lw.size() == 1 && ((lw[0] >= 'a' && lw[0] <= 'z'));
                for (const char* a : kAbbrev)
                    if (lw == a) { abbrev = true; break; }
                if (abbrev && i < text.size()) continue;
            }
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        }
    }
    const std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

bool NmtEngine::translateDirect(const std::string& text, const std::string& src, const std::string& tgt,
                                std::string& out, std::string* error) {
    out.clear();
    const std::string input = trim(text);
    if (input.empty()) return true;

    if (!loadPair(src, tgt, error)) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    LoadedPair& p = *impl_->loaded[pairName(src, tgt)];

    const std::vector<std::string> sentences = splitSentences(input);
    std::vector<std::vector<std::string>> batch;
    batch.reserve(sentences.size());
    for (const auto& s : sentences) batch.push_back(impl_->encode(p, s));

    std::vector<std::string> translated;
#if TRANSLATOR_HAS_CTRANSLATE2
    try {
        ctranslate2::TranslationOptions opts;
        opts.beam_size = static_cast<std::size_t>(impl_->beamSize);
        opts.max_decoding_length = static_cast<std::size_t>(impl_->maxDecodingLength);
        opts.return_scores = false;
        opts.repetition_penalty = impl_->repetitionPenalty;
        opts.no_repeat_ngram_size = static_cast<std::size_t>(impl_->noRepeatNgramSize);
        // Bound decoding by input length: Marian never needs more than ~3x the source tokens.
        std::size_t longest = 0;
        for (const auto& t : batch) longest = std::max(longest, t.size());
        opts.max_decoding_length = std::min<std::size_t>(opts.max_decoding_length, longest * 3 + 16);

        std::vector<std::vector<std::string>> prefixes;
        if (!p.targetPrefixToken.empty()) prefixes.assign(batch.size(), {p.targetPrefixToken});

        const auto results = prefixes.empty() ? p.translator->translate_batch(batch, opts)
                                              : p.translator->translate_batch(batch, prefixes, opts);
        for (const auto& r : results) translated.push_back(impl_->decode(p, r.output()));
    } catch (const std::exception& e) {
        if (error) *error = std::string("CTranslate2 translate failed: ") + e.what();
        return false;
    }
#else
    // Stub build: echo the tokens back so the pipeline can be exercised end-to-end.
    for (const auto& tokens : batch) translated.push_back("[" + pairName(src, tgt) + "] " + impl_->decode(p, tokens));
#endif

    const std::string sep = isCjk(tgt) ? "" : " ";
    for (std::size_t i = 0; i < translated.size(); ++i) {
        std::string t = trim(translated[i]);
        if (t.empty()) continue;
        if (tgt == "ko") {
            // Marian renders a bare "Hello." as the phone greeting "여보세요?"; in conversation
            // the face-to-face greeting is meant.
            std::string lowerSrc;
            for (const char c : sentences[i]) lowerSrc.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
            const bool greeting = lowerSrc == "hello." || lowerSrc == "hello" || lowerSrc == "hello!" || lowerSrc == "hi." || lowerSrc == "hi" || lowerSrc == "hi!";
            if (greeting && t.find("\xEC\x97\xAC\xEB\xB3\xB4\xEC\x84\xB8\xEC\x9A\x94") == 0) t = "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94.";
        }
        if (isCjk(tgt)) {
            // Some ja/zh models drop sentence-final punctuation; restore it from the source
            // sentence so consecutive sentences don't run together ("こんにちはいい天気だ").
            static const char* const kTerminals[] = {".", "!", "?", "\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F"};
            bool ends = false;
            for (const char* term : kTerminals)
                if (t.size() >= std::strlen(term) && t.compare(t.size() - std::strlen(term), std::strlen(term), term) == 0) ends = true;
            if (!ends) {
                const std::string& srcSent = sentences[i];
                const bool q = !srcSent.empty() && (srcSent.back() == '?' || (srcSent.size() >= 3 && srcSent.compare(srcSent.size() - 3, 3, "\xEF\xBC\x9F") == 0));
                const bool ex = !srcSent.empty() && (srcSent.back() == '!' || (srcSent.size() >= 3 && srcSent.compare(srcSent.size() - 3, 3, "\xEF\xBC\x81") == 0));
                t += q ? "\xEF\xBC\x9F" : ex ? "\xEF\xBC\x81" : "\xE3\x80\x82";
            }
        }
        if (!out.empty()) out += sep;
        out += t;
    }
    if (error) error->clear();
    return true;
}

bool NmtEngine::translate(const std::string& text, const std::string& src, const std::string& tgt,
                          const std::vector<std::string>& pivots, std::string& out,
                          std::vector<std::string>* routeOut, std::string* error) {
    if (routeOut) routeOut->clear();
    if (src == tgt) {
        out = text;
        return true;
    }
    const auto route = resolveRoute(src, tgt, pivots, error);
    if (route.empty()) return false;

    std::string cur = text;
    for (const auto& hop : route) {
        std::string next;
        if (!translateDirect(cur, hop.first, hop.second, next, error)) return false;
        if (routeOut) routeOut->push_back(pairName(hop.first, hop.second));
        cur = std::move(next);
    }
    out = std::move(cur);
    return true;
}

} // namespace translator
