// Dependency-free unit tests for the pure C++ core (run on desktop, no models needed).
#include "translator/FileUtil.hpp"
#include "translator/LlmEngine.hpp"
#include "translator/MiniJson.hpp"
#include "translator/ModelManager.hpp"
#include "translator/NmtEngine.hpp"
#include "translator/Sha256.hpp"
#include "translator/SpeechSegmenter.hpp"
#include "translator/TextUtil.hpp"
#include "translator/TranslationPipeline.hpp"
#include "translator_c_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace translator;

namespace {

int gFailures = 0;
int gChecks = 0;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        ++gChecks;                                                                            \
        if (!(cond)) {                                                                        \
            ++gFailures;                                                                      \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        ++gChecks;                                                                            \
        if (!((a) == (b))) {                                                                  \
            ++gFailures;                                                                      \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);     \
        }                                                                                     \
    } while (0)

std::string tempRoot() {
    const char* base = std::getenv("TRANSLATOR_TEST_TMP");
    std::string root = base ? base : "";
    if (root.empty()) {
#if defined(_WIN32)
        const char* t = std::getenv("TEMP");
        root = t ? t : ".";
#else
        root = "/tmp";
#endif
    }
    root = fs::join(root, "translator_tests");
    fs::removeAll(root);
    fs::makeDirs(root);
    return root;
}

// ---------------------------------------------------------------------------

void testSha256() {
    std::puts("[sha256]");
    CHECK_EQ(Sha256::hashString(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(Sha256::hashString("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(Sha256::hashString("The quick brown fox jumps over the lazy dog"),
             "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    // 56-byte message exercises the "padding spills into a second block" path.
    CHECK_EQ(Sha256::hashString("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a' streamed in odd-sized chunks.
    Sha256 h;
    const std::string chunk(997, 'a');
    std::size_t remaining = 1000000;
    while (remaining) {
        const std::size_t n = std::min(remaining, chunk.size());
        h.update(chunk.data(), n);
        remaining -= n;
    }
    CHECK_EQ(h.hexDigest(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::string root = tempRoot();
    const std::string f = fs::join(root, "hash.bin");
    std::string data;
    for (int i = 0; i < 300000; ++i) data.push_back(static_cast<char>(i * 7));
    CHECK(fs::writeFileAtomic(f, data));
    std::uint64_t lastTotal = 0;
    const std::string fileHash = Sha256::hashFile(f, [&](std::uint64_t, std::uint64_t total) {
        lastTotal = total;
        return true;
    });
    CHECK_EQ(fileHash, Sha256::hashString(data));
    CHECK_EQ(lastTotal, static_cast<std::uint64_t>(data.size()));
    CHECK(Sha256::hashFile(fs::join(root, "does-not-exist")).empty());
    CHECK(Sha256::equalsIgnoreCase("ABC", "abc"));
}

void testJson() {
    std::puts("[json]");
    std::string err;
    const Json j = Json::parse(R"({"a": 1, "b": [true, null, "xé😀"], "c": {"d": -2.5e1}, "e": ""})", &err);
    CHECK(err.empty());
    CHECK(j.isObject());
    CHECK_EQ(j["a"].asInt(), 1);
    CHECK_EQ(j["b"].size(), std::size_t(3));
    CHECK(j["b"].at(0).asBool());
    CHECK(j["b"].at(1).isNull());
    CHECK_EQ(j["b"].at(2).asString(), std::string("x\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK_EQ(j["c"]["d"].asNumber(), -25.0);
    CHECK(j["missing"].isNull());
    CHECK_EQ(j.getString("missing", "def"), "def");
    CHECK_EQ(j["e"].asString(), "");

    // Round trip (compact) — insertion order preserved.
    const std::string dumped = j.dump();
    const Json back = Json::parse(dumped, &err);
    CHECK(err.empty());
    CHECK_EQ(back.dump(), dumped);
    CHECK_EQ(Json(482344960LL).dump(), "482344960");
    CHECK_EQ(Json(1.5).dump(), "1.5");
    CHECK_EQ(Json("q\"\\\n").dump(), "\"q\\\"\\\\\\n\"");

    // Errors.
    Json::parse("{\"a\":}", &err);
    CHECK(!err.empty());
    Json::parse("[1,2", &err);
    CHECK(!err.empty());
    Json::parse("{} x", &err);
    CHECK(!err.empty());
    Json::parse("\xEF\xBB\xBF{\"bom\":true}", &err);
    CHECK(err.empty());

    // Mutation.
    Json o = Json::object();
    o.set("k", "v");
    o["arr"].push_back(1);
    o["arr"].push_back(2);
    CHECK_EQ(o.dump(), "{\"k\":\"v\",\"arr\":[1,2]}");
    CHECK_EQ(o.dump(2), "{\n  \"k\": \"v\",\n  \"arr\": [\n    1,\n    2\n  ]\n}");
}

void testSegmenter() {
    std::puts("[segmenter]");
    SegmenterConfig cfg;
    cfg.endSilenceMs = 300;
    cfg.minUtteranceMs = 300;
    cfg.maxUtteranceMs = 2000;
    cfg.preRollMs = 90; // 3 frames
    SpeechSegmenter seg(cfg);

    std::vector<float> speech(kFrameSize, 0.5f), silence(kFrameSize, 0.0f);
    auto push = [&](int frames, bool voiced) {
        bool ready = false;
        for (int i = 0; i < frames; ++i) ready |= seg.pushFrame(voiced ? speech.data() : silence.data(), voiced ? 0.95f : 0.05f);
        return ready;
    };

    CHECK(!push(20, false));          // 600 ms silence
    CHECK(!push(30, true));           // 900 ms speech → in speech
    CHECK(seg.inSpeech());
    CHECK(push(15, false));           // 450 ms silence → closes
    CHECK(seg.hasUtterance());
    std::vector<float> u = seg.popUtterance();
    // 3 pre-roll frames (silence) + 30 speech frames + ~150 ms retained silence.
    CHECK(u.size() >= std::size_t(33) * kFrameSize);
    CHECK(u.size() <= std::size_t(33 + 6) * kFrameSize);
    CHECK(std::fabs(u[std::size_t(3) * kFrameSize] - 0.5f) < 1e-6f); // speech starts after pre-roll
    CHECK(!seg.hasUtterance());

    // Too short: 2 speech frames then silence → discarded.
    CHECK(!push(2, true));
    CHECK(!push(15, false));
    CHECK(!seg.hasUtterance());

    // Max length split: 100 frames = 3 s continuous speech → forced close at 2 s.
    push(100, true);
    CHECK(seg.hasUtterance());
    u = seg.popUtterance();
    const std::size_t maxSamples = static_cast<std::size_t>(cfg.maxUtteranceMs) * kSampleRate / 1000;
    CHECK(u.size() >= maxSamples);                  // closes at frame granularity...
    CHECK(u.size() < maxSamples + kFrameSize);      // ...never more than one frame over

    // Flush closes the rest.
    CHECK(seg.flush());
    CHECK(seg.hasUtterance());
    seg.reset();
    CHECK(!seg.hasUtterance());
}

void testTextUtil() {
    std::puts("[text util]");
    using namespace translator::text;

    // Prompts
    CHECK(!defaultPromptFor("ko").empty());
    CHECK(defaultPromptFor("xx").empty());
    CHECK_EQ(buildSttPrompt("", "ko", ""), defaultPromptFor("ko"));
    CHECK_EQ(buildSttPrompt("custom", "ko", ""), "custom");
    const std::string withCtx = buildSttPrompt("", "en", "the previous sentence was here.");
    CHECK(withCtx.find(defaultPromptFor("en")) == 0);
    CHECK(withCtx.find("previous sentence") != std::string::npos);
    // Long context is truncated at a word boundary from the end.
    std::string longCtx;
    for (int i = 0; i < 100; ++i) longCtx += "word" + std::to_string(i) + " ";
    const std::string truncated = buildSttPrompt("x", "", longCtx, 50);
    CHECK(truncated.size() < 60);
    CHECK(truncated.find("word99") != std::string::npos);
    CHECK(truncated.find("x ") == 0);

    // Hallucinations
    CHECK(isHallucination("시청해주셔서 감사합니다.", "ko"));
    CHECK(isHallucination("Thank you for watching!", "en"));
    CHECK(isHallucination("ご視聴ありがとうございました", "ja"));
    CHECK(isHallucination("Thanks for watching.", "auto"));
    CHECK(!isHallucination("오늘 날씨가 정말 좋네요.", "ko"));
    CHECK(!isHallucination("Thank you for the detailed report on the quarterly numbers.", "en"));

    // Cleaning
    CHECK_EQ(cleanTranscript("[음악] 안녕하세요 (박수) 반갑습니다", "ko"), "안녕하세요 반갑습니다");
    CHECK_EQ(cleanTranscript("♪ ♪ ♪", "en"), "");
    CHECK_EQ(cleanTranscript("yes yes yes yes yes yes yes no", "en"), "yes yes yes no");
    CHECK_EQ(cleanTranscript("감사합니다감사합니다감사합니다감사합니다감사합니다 네", "ko"), "감사합니다감사합니다 네");
    CHECK_EQ(cleanTranscript("시청해주셔서 감사합니다", "ko"), "");
    CHECK_EQ(cleanTranscript("  Hello   world  ", "en"), "Hello world");

    // Post-processing
    CHECK_EQ(postProcessTranslation("hello . it 's a great day . how are you?", "en"), "Hello. It 's a great day. How are you?");
    CHECK_EQ(postProcessTranslation("the value is 3.14 , ok", "en"), "The value is 3.14, ok");
    CHECK_EQ(postProcessTranslation("the meeting starts at 3:00 p.m. see you e.g. tomorrow.", "en"),
             "The meeting starts at 3:00 p.m. See you e.g. tomorrow.");
    CHECK_EQ(postProcessTranslation("안녕하세요 . 반갑습니다 !", "ko"), "안녕하세요. 반갑습니다!");
    CHECK_EQ(postProcessTranslation("시작됩니다.이 제품은 3.5kg입니다.", "ko"), "시작됩니다. 이 제품은 3.5kg입니다.");
    CHECK_EQ(postProcessTranslation("こんにちは 。 元気 です か ?", "ja"), "こんにちは。元気ですか？");
    CHECK_EQ(postProcessTranslation("你好 , 世界 .", "zh"), "你好，世界。");
    CHECK_EQ(postProcessTranslation("¿cómo estás? bien.", "es"), "¿Cómo estás? Bien.");
    CHECK(isCjkChar("한"));
    CHECK(isCjkChar("漢"));
    CHECK(!isCjkChar("a"));
}

void testLlmEngine() {
    std::puts("[llm engine]");
    CHECK_EQ(LlmEngine::languageName("vi"), "Vietnamese");
    CHECK_EQ(LlmEngine::languageName("th"), "Thai");
    CHECK_EQ(LlmEngine::languageName("xx"), "xx");
    const std::string p = LlmEngine::buildInstruction("ko", "en", "");
    CHECK(p.find("from Korean into English") != std::string::npos);
    CHECK(p.find("ONLY the English translation") != std::string::npos);
    const std::string pa = LlmEngine::buildInstruction("auto", "th", "");
    CHECK(pa.find("Detect the language") != std::string::npos);
    CHECK(pa.find("into Thai") != std::string::npos);
    CHECK_EQ(LlmEngine::buildInstruction("ko", "en", "custom"), "custom");
    CHECK(!LlmEngine::exampleSentence("th").empty());
    CHECK(LlmEngine::exampleSentence("xx").empty());
    CHECK(LlmEngine::foreignScriptRatio("안녕하세요 반갑습니다", "ko") < 0.01);
    CHECK(LlmEngine::foreignScriptRatio("안녕하세요 户外使用 밝기", "ko") > 0.15);
    CHECK(LlmEngine::foreignScriptRatio("สวัสดีครับ 会议明天开始", "th") > 0.3);
    CHECK(LlmEngine::foreignScriptRatio("Hello p.m. 3", "en") < 0.01);
    CHECK(LlmEngine::foreignScriptRatio("東京駅はどこですか", "ja") < 0.01);
    CHECK(LlmEngine::foreignScriptRatio("", "ko") == 0.0);

    LlmEngine e;
    CHECK(!e.isLoaded());
    CHECK(!e.load(fs::join(tempRoot(), "missing.gguf"), LlmOptions{}) || !LlmEngine::isNativeLlama());
    LlmResult r = e.translate("", "ko", "en");
    CHECK(r.ok && r.text.empty());
    r = e.translate("안녕", "ko", "en");
    CHECK(!r.ok);
    CHECK(!r.error.empty());

    // Languages
    CHECK_EQ(supportedLanguages().size(), std::size_t(7));
    CHECK(isSupportedLanguage("vi"));
    CHECK(isSupportedLanguage("th"));
    CHECK(!text::defaultPromptFor("vi").empty());
    CHECK(!text::defaultPromptFor("th").empty());
    CHECK(text::isHallucination("ขอบคุณที่รับชม", "th"));
    CHECK(text::isHallucination("Cảm ơn các bạn đã theo dõi!", "vi"));
    CHECK_EQ(text::postProcessTranslation("สวัสดี  ครับ", "th"), "สวัสดี ครับ");
    CHECK_EQ(text::postProcessTranslation("xin chào . tôi là", "vi"), "Xin chào. Tôi là");
}

void testSentenceSplit() {
    std::puts("[sentence split]");
    auto s = NmtEngine::splitSentences("Hello there. How are you? I am fine!  Pi is 3.14 ok.\n안녕하세요。잘 지내세요？");
    CHECK_EQ(s.size(), std::size_t(6));
    if (s.size() == 6) {
        CHECK_EQ(s[0], "Hello there.");
        CHECK_EQ(s[1], "How are you?");
        CHECK_EQ(s[2], "I am fine!");
        CHECK_EQ(s[3], "Pi is 3.14 ok.");
        CHECK_EQ(s[4], "안녕하세요。");
        CHECK_EQ(s[5], "잘 지내세요？");
    }
    CHECK(NmtEngine::splitSentences("   ").empty());
    const auto abbr = NmtEngine::splitSentences("It is 3 p.m. now. See Mr. Kim, e.g. tomorrow. Bye.");
    CHECK_EQ(abbr.size(), std::size_t(3));
    if (abbr.size() == 3) {
        CHECK_EQ(abbr[0], "It is 3 p.m. now.");
        CHECK_EQ(abbr[1], "See Mr. Kim, e.g. tomorrow.");
        CHECK_EQ(abbr[2], "Bye.");
    }
}

std::string makeManifest(const std::string& sttData, const std::string& modelData, const std::string& vocabData,
                         const std::string& sttVersion = "1") {
    Json m = Json::object();
    m.set("manifest_version", "1.1.0");
    m.set("base_url", "https://assets.example.com/models");

    Json stt = Json::object();
    stt.set("id", "whisper-tiny-test");
    stt.set("version", sttVersion);
    stt.set("filename", "whisper-tiny-test.bin");
    stt.set("size_bytes", static_cast<std::int64_t>(sttData.size()));
    stt.set("sha256", Sha256::hashString(sttData));
    stt.set("download_url", "stt/whisper-tiny-test.bin"); // relative → base_url
    m.set("stt", stt);

    Json nmt = Json::array();
    {
        Json p = Json::object();
        p.set("pair", "ko-en");
        p.set("version", "1");
        Json files = Json::array();
        Json f1 = Json::object();
        f1.set("filename", "model.bin");
        f1.set("size_bytes", static_cast<std::int64_t>(modelData.size()));
        f1.set("sha256", Sha256::hashString(modelData));
        files.push_back(f1);
        Json f2 = Json::object();
        f2.set("filename", "shared_vocabulary.json");
        f2.set("size_bytes", static_cast<std::int64_t>(vocabData.size()));
        f2.set("sha256", Sha256::hashString(vocabData));
        files.push_back(f2);
        p.set("files", files);
        nmt.push_back(p);
    }
    {
        Json p = Json::object(); // zip mode, README-style
        p.set("pair", "en-ko");
        p.set("dir_name", "en-ko");
        p.set("size_bytes", 12345);
        p.set("sha256", "ef2d127de37b942baad06145e54b0c619a1f22327b2ebbcfbec78f5564afe39d");
        p.set("download_url", "https://assets.example.com/models/nmt/en-ko.zip");
        nmt.push_back(p);
    }
    m.set("nmt", nmt);

    Json llm = Json::object();
    llm.set("id", "qwen-test");
    llm.set("version", "1");
    llm.set("filename", "qwen-test.gguf");
    llm.set("size_bytes", 4);
    llm.set("sha256", Sha256::hashString("gguf"));
    llm.set("download_url", "llm/qwen-test.gguf");
    m.set("llm", llm);
    return m.dump(2);
}

void testModelManager() {
    std::puts("[model manager]");
    const std::string root = fs::join(tempRoot(), "models");
    const std::string sttData(5000, 'S'), modelData(3000, 'M'), vocabData = "{\"vocab\":[]}";

    ModelManager mm(root);
    std::string err;
    CHECK(mm.loadManifestJson(makeManifest(sttData, modelData, vocabData), &err));
    CHECK(err.empty());
    CHECK_EQ(mm.entries().size(), std::size_t(4));
    CHECK_EQ(mm.manifestVersion(), "1.1.0");
    const ModelEntry* llmEntry = mm.find("qwen-test");
    CHECK(llmEntry != nullptr);
    if (llmEntry) {
        CHECK(llmEntry->kind == ModelEntry::Kind::Llm);
        CHECK(llmEntry->isSingleFile());
        CHECK_EQ(llmEntry->installPath, fs::join(fs::join(root, "llm"), "qwen-test.gguf"));
        CHECK_EQ(llmEntry->downloads.front().url, "https://assets.example.com/models/llm/qwen-test.gguf");
    }
    CHECK_EQ(mm.llmModelPath(), fs::join(fs::join(root, "llm"), "qwen-test.gguf"));

    const ModelEntry* stt = mm.find("whisper-tiny-test");
    CHECK(stt != nullptr);
    if (stt) {
        CHECK_EQ(stt->downloads.front().url, "https://assets.example.com/models/stt/whisper-tiny-test.bin");
        CHECK_EQ(stt->installPath, fs::join(fs::join(root, "stt"), "whisper-tiny-test.bin"));
        CHECK(!stt->downloads.front().isArchive);
    }
    const ModelEntry* koen = mm.find("nmt-ko-en");
    CHECK(koen != nullptr);
    if (koen) {
        CHECK_EQ(koen->downloads.size(), std::size_t(2));
        CHECK_EQ(koen->downloads[0].url, "https://assets.example.com/models/nmt/ko-en/model.bin");
        CHECK_EQ(koen->requiredFiles.size(), std::size_t(2));
    }
    const ModelEntry* enko = mm.find("nmt-en-ko");
    CHECK(enko != nullptr);
    if (enko) {
        CHECK(enko->downloads.front().isArchive);
        CHECK_EQ(enko->downloads.front().filename, "en-ko.zip");
        CHECK_EQ(enko->requiredFiles.size(), std::size_t(1));
    }

    // Everything missing initially.
    for (const auto& s : mm.status()) CHECK(s.state == ModelState::Missing);
    CHECK_EQ(mm.pending().size(), std::size_t(4));
    const Json sj = mm.statusJson();
    CHECK_EQ(sj["models"].size(), std::size_t(4));
    CHECK_EQ(sj["pending_bytes"].asInt64(), static_cast<std::int64_t>(sttData.size() + modelData.size() + vocabData.size() + 12345 + 4));

    // LLM inclusion: ko+en have direct pairs → not needed; ko+ja has no route → needed.
    auto hasLlm = [](const std::vector<ModelStatus>& v) {
        for (const auto& s : v) if (s.entry.kind == ModelEntry::Kind::Llm) return true;
        return false;
    };
    CHECK(!hasLlm(mm.statusForLanguages({"ko", "en"})));
    CHECK(hasLlm(mm.statusForLanguages({"ko", "ja"})));
    CHECK(hasLlm(mm.statusForLanguages({"ko", "en"}, false, ModelManager::LlmMode::Always)));
    CHECK(!hasLlm(mm.statusForLanguages({"ko", "ja"}, false, ModelManager::LlmMode::Never)));

    // Install the LLM file like the STT file.
    const std::string llmStaging = mm.stagingDir("qwen-test");
    CHECK(fs::writeFileAtomic(fs::join(llmStaging, "qwen-test.gguf"), "gguf"));
    CHECK(mm.install("qwen-test", llmStaging, true, nullptr, &err));
    CHECK(mm.statusOf(*mm.find("qwen-test"), true).state == ModelState::Ready);

    // Stage + install STT (file directly in staging dir).
    const std::string sttStaging = mm.stagingDir("whisper-tiny-test");
    CHECK(fs::writeFileAtomic(fs::join(sttStaging, "whisper-tiny-test.bin"), sttData));
    CHECK(mm.verifyFile(fs::join(sttStaging, "whisper-tiny-test.bin"), stt->downloads.front().sha256, sttData.size()) == VerifyResult::Ok);
    CHECK(mm.verifyFile(fs::join(sttStaging, "whisper-tiny-test.bin"), "00", sttData.size()) == VerifyResult::HashMismatch);
    CHECK(mm.verifyFile(fs::join(sttStaging, "whisper-tiny-test.bin"), "", 1) == VerifyResult::SizeMismatch);
    CHECK(mm.install("whisper-tiny-test", sttStaging, true, nullptr, &err));
    CHECK(err.empty());
    CHECK(fs::isFile(stt->installPath));
    CHECK(!fs::exists(sttStaging));
    CHECK(mm.statusOf(*stt).state == ModelState::Ready);
    CHECK(mm.statusOf(*stt, true).state == ModelState::Ready);
    CHECK_EQ(mm.sttModelPath(), stt->installPath);

    // Install with a wrong hash must fail and leave nothing behind.
    const std::string bad = mm.stagingDir("whisper-tiny-test");
    CHECK(fs::writeFileAtomic(fs::join(bad, "whisper-tiny-test.bin"), std::string(5000, 'X')));
    CHECK(!mm.install("whisper-tiny-test", bad, true, nullptr, &err));
    CHECK(!err.empty());
    CHECK(mm.statusOf(*stt, true).state == ModelState::Ready); // old file untouched
    mm.clearStaging();

    // Files-mode NMT install.
    const std::string koStaging = mm.stagingDir("nmt-ko-en");
    CHECK(fs::writeFileAtomic(fs::join(koStaging, "model.bin"), modelData));
    CHECK(fs::writeFileAtomic(fs::join(koStaging, "shared_vocabulary.json"), vocabData));
    CHECK(mm.install("nmt-ko-en", koStaging, true, nullptr, &err));
    CHECK(err.empty());
    CHECK(fs::isFile(fs::join(koen->installPath, "model.bin")));
    CHECK(mm.statusOf(*koen, true).state == ModelState::Ready);

    // Zip-mode NMT install from an extracted directory with a nested folder.
    const std::string enStaging = mm.stagingDir("nmt-en-ko");
    CHECK(fs::writeFileAtomic(fs::join(fs::join(enStaging, "en-ko"), "model.bin"), "zip-model"));
    CHECK(mm.install("nmt-en-ko", enStaging, true, nullptr, &err));
    CHECK(err.empty());
    CHECK(fs::isFile(fs::join(enko->installPath, "model.bin")));
    CHECK(mm.statusOf(*enko).state == ModelState::Ready);
    CHECK_EQ(mm.pending().size(), std::size_t(0));

    // Deep verify detects same-size tampering.
    std::string tampered = modelData;
    tampered[10] = 'Z';
    CHECK(fs::writeFileAtomic(fs::join(koen->installPath, "model.bin"), tampered));
    CHECK(mm.statusOf(*koen).state == ModelState::Ready);        // quick check can't see it
    CHECK(mm.statusOf(*koen, true).state == ModelState::Corrupt); // deep check can
    CHECK(fs::writeFileAtomic(fs::join(koen->installPath, "model.bin"), modelData + "extra"));
    CHECK(mm.statusOf(*koen).state == ModelState::Corrupt);       // size mismatch is quick

    // Manifest update → update_available.
    CHECK(mm.loadManifestJson(makeManifest(sttData, modelData, vocabData, "2"), &err));
    CHECK(mm.statusOf(*mm.find("whisper-tiny-test")).state == ModelState::UpdateAvailable);

    // Language filtering.
    const auto forKoEn = mm.statusForLanguages({"ko", "en"});
    CHECK_EQ(forKoEn.size(), std::size_t(3));
    const auto forKoJa = mm.statusForLanguages({"ko", "ja"});
    CHECK_EQ(forKoJa.size(), std::size_t(2)); // STT + LLM (no ko-ja route)

    // Cache + reload.
    CHECK(mm.saveManifest(&err));
    ModelManager mm2(root);
    CHECK(mm2.loadCachedManifest(&err));
    CHECK_EQ(mm2.entries().size(), std::size_t(4));

    // Remove.
    CHECK(mm2.remove("nmt-en-ko", &err));
    CHECK(mm2.statusOf(*mm2.find("nmt-en-ko")).state == ModelState::Missing);

    // Unverified: files present without record.
    fs::remove(mm2.installedRecordPath());
    CHECK(mm2.statusOf(*mm2.find("whisper-tiny-test")).state == ModelState::Unverified);
    CHECK(mm2.statusOf(*mm2.find("whisper-tiny-test"), true).state == ModelState::Ready);

    // Bad manifests.
    CHECK(!mm2.loadManifestJson("{", &err));
    CHECK(!mm2.loadManifestJson("{\"nmt\":[{\"dir_name\":\"x\"}]}", &err));
    CHECK(!mm2.loadManifestJson("{}", &err));
}

void testPipelineStub() {
    std::puts("[pipeline]");
    const std::string root = fs::join(tempRoot(), "models");
    const std::string nmt = fs::join(root, "nmt");
    // Fake pairs (no real model; NMT stub echoes tokens).
    CHECK(fs::writeFileAtomic(fs::join(fs::join(nmt, "ja-ko"), "model.bin"), "x"));
    CHECK(fs::writeFileAtomic(fs::join(fs::join(nmt, "ko-en"), "model.bin"), "x"));
    CHECK(fs::writeFileAtomic(fs::join(fs::join(nmt, "ko-en"), "pair.json"), "{\"source_prefix_token\":\">>eng<<\"}"));
    CHECK(fs::writeFileAtomic(fs::join(fs::join(nmt, "not-a-pair-dir"), "model.bin"), "x"));

    PipelineConfig cfg;
    cfg.nmtRootDir = nmt;
    cfg.enableDenoise = true; // exercises the fallback energy VAD when RNNoise is absent
    cfg.pivotLangs = {"ko", "en"};

    TranslationPipeline p;
    std::string err;
    CHECK(p.initialize(cfg, &err));
    CHECK(err.empty());
    CHECK(p.isInitialized());

    const auto pairs = p.availablePairs();
    CHECK_EQ(pairs.size(), std::size_t(2));
    CHECK(p.canTranslate("ko", "en"));
    CHECK(p.canTranslate("ja", "en"));   // via ko
    CHECK(!p.canTranslate("es", "en"));
    CHECK(p.canTranslate("en", "en"));

    const Json caps = p.capabilities();
    CHECK(caps["initialized"].asBool());
    CHECK_EQ(caps["available_pairs"].size(), std::size_t(2));

#if !TRANSLATOR_HAS_CTRANSLATE2
    TranslationResult r = p.translateText("안녕하세요 세계", "ko", "en");
    CHECK(r.ok);
    CHECK_EQ(r.route.size(), std::size_t(1));
    CHECK(r.translatedText.find("[ko-en]") != std::string::npos);
    CHECK(r.translatedText.find(">>eng<<") != std::string::npos); // prefix token applied

    r = p.translateText("こんにちは", "ja", "en");
    CHECK(r.ok);
    CHECK_EQ(r.route.size(), std::size_t(2));
    if (r.route.size() == 2) {
        CHECK_EQ(r.route[0], "ja-ko");
        CHECK_EQ(r.route[1], "ko-en");
    }
#endif
    TranslationResult bad = p.translateText("hola", "es", "en");
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
    CHECK(!p.lastError().empty());

    TranslationResult same = p.translateText("same", "en", "en");
    CHECK(same.ok);
    CHECK_EQ(same.translatedText, "same");

    const std::string json = same.toJson().dump();
    CHECK(Json::parse(json)["ok"].asBool());

    // Streaming: 1 s silence, 1.5 s tone, 1.5 s silence, fed in odd chunk sizes.
    std::vector<float> audio;
    for (int i = 0; i < kSampleRate; ++i) audio.push_back(0.0005f * static_cast<float>((i % 7) - 3));
    for (int i = 0; i < kSampleRate * 3 / 2; ++i) audio.push_back(0.4f * std::sin(2.0f * 3.14159f * 220.0f * i / kSampleRate));
    for (int i = 0; i < kSampleRate * 3 / 2; ++i) audio.push_back(0.0005f * static_cast<float>((i % 5) - 2));

    bool ready = false;
    for (std::size_t i = 0; i < audio.size(); i += 333) {
        const std::size_t n = std::min<std::size_t>(333, audio.size() - i);
        ready |= p.feedAudio(audio.data() + i, n);
    }
    CHECK(ready);
    CHECK(p.hasPendingUtterance());
    const std::vector<float> utt = p.popPendingUtterance();
    CHECK(utt.size() >= std::size_t(kSampleRate));           // ≥ 1 s of speech captured
    CHECK(utt.size() <= std::size_t(kSampleRate) * 5 / 2);   // but not the whole stream
    CHECK(!p.hasPendingUtterance());

    // Without whisper compiled in, processing reports a clear error rather than crashing.
    TranslationResult sp = p.processSpeechToTranslation(utt.data(), utt.size(), "auto", "en");
#if TRANSLATOR_HAS_WHISPER
    CHECK(!sp.ok); // no model loaded
#else
    CHECK(!sp.ok);
    CHECK(sp.error.find("whisper") != std::string::npos);
#endif

    // Missing whisper file is rejected at init.
    PipelineConfig bad2 = cfg;
    bad2.whisperModelPath = fs::join(root, "nope.bin");
    TranslationPipeline p2;
    CHECK(!p2.initialize(bad2, &err));
    CHECK(!err.empty());
}

int progressCalls = 0;
int progressCb(uint64_t, uint64_t, void* user) {
    ++*static_cast<int*>(user);
    return 1;
}

void testCApi() {
    std::puts("[c api]");
    const std::string root = fs::join(tempRoot(), "models_c");
    CHECK(std::string(tr_version()).size() > 0);
    char* caps = tr_build_capabilities();
    CHECK(caps != nullptr);
    if (caps) {
        const Json c = Json::parse(caps);
        CHECK(c.contains("whisper"));
        tr_string_free(caps);
    }

    tr_model_manager* mm = tr_mm_create(root.c_str());
    CHECK(mm != nullptr);
    const std::string sttData(100, 'a');
    const std::string manifest = makeManifest(sttData, "m", "v");
    CHECK_EQ(tr_mm_load_manifest_json(mm, manifest.c_str()), 1);
    CHECK_EQ(tr_mm_load_manifest_json(mm, "nope"), 0);
    CHECK(std::string(tr_mm_last_error(mm)).find("parse") != std::string::npos);
    CHECK_EQ(tr_mm_load_manifest_json(mm, manifest.c_str()), 1);
    CHECK_EQ(std::string(tr_mm_manifest_version(mm)), "1.1.0");

    char* status = tr_mm_status_json(mm, 0);
    const Json sj = Json::parse(status ? status : "");
    tr_string_free(status);
    CHECK_EQ(sj["models"].size(), std::size_t(4));
    CHECK_EQ(sj["models"].at(0)["state"].asString(), "missing");
    CHECK(sj["models"].at(0)["needs_download"].asBool());

    char* staging = tr_mm_staging_dir(mm, "whisper-tiny-test");
    CHECK(staging != nullptr);
    const std::string stagedFile = fs::join(staging ? staging : "", "whisper-tiny-test.bin");
    tr_string_free(staging);
    CHECK(fs::writeFileAtomic(stagedFile, sttData));
    int calls = 0;
    CHECK_EQ(tr_mm_verify_file(mm, stagedFile.c_str(), Sha256::hashString(sttData).c_str(), sttData.size(), progressCb, &calls), 1);
    CHECK(calls > 0);
    CHECK_EQ(tr_mm_verify_file(mm, stagedFile.c_str(), "deadbeef", 0, nullptr, nullptr), 0);
    CHECK_EQ(tr_mm_verify_file(mm, "/nonexistent/file", "", 0, nullptr, nullptr), -1);
    CHECK_EQ(tr_mm_install(mm, "whisper-tiny-test", stagedFile.c_str(), 1, nullptr, nullptr), 1);

    status = tr_mm_status_for_languages_json(mm, "ko,en", 1, 2);
    const Json sj2 = Json::parse(status ? status : "");
    tr_string_free(status);
    CHECK_EQ(sj2["models"].at(0)["state"].asString(), "ready");

    char* sttPath = tr_mm_stt_model_path(mm);
    CHECK(sttPath && fs::isFile(sttPath));
    tr_string_free(sttPath);

    char* hex = tr_sha256_file(fs::join(fs::join(root, "stt"), "whisper-tiny-test.bin").c_str(), nullptr, nullptr);
    CHECK(hex && Sha256::hashString(sttData) == hex);
    tr_string_free(hex);
    tr_mm_destroy(mm);

    // Pipeline via C API (text-only, stub NMT).
    char* nmtRoot = nullptr;
    {
        tr_model_manager* mm2 = tr_mm_create(root.c_str());
        nmtRoot = tr_mm_nmt_root_dir(mm2);
        tr_mm_destroy(mm2);
    }
    CHECK(fs::writeFileAtomic(fs::join(fs::join(nmtRoot, "ko-en"), "model.bin"), "x"));
    tr_pipeline_config cfg;
    tr_pipeline_config_init(&cfg);
    cfg.nmt_root_dir = nmtRoot;
    cfg.pivot_langs = "ko, en";
    tr_pipeline* p = tr_pipeline_create(&cfg);
    CHECK(p != nullptr);
    if (p) {
        CHECK_EQ(tr_pipeline_can_translate(p, "ko", "en"), 1);
        CHECK_EQ(tr_pipeline_can_translate(p, "zh", "en"), 0);
        char* pairs = tr_pipeline_available_pairs(p);
        CHECK_EQ(Json::parse(pairs).size(), std::size_t(1));
        tr_string_free(pairs);

        char* res = tr_pipeline_translate_text(p, "테스트", "ko", "en");
        const Json rj = Json::parse(res ? res : "");
        tr_string_free(res);
#if !TRANSLATOR_HAS_CTRANSLATE2
        CHECK(rj["ok"].asBool());
#endif
        CHECK_EQ(rj["source_lang"].asString(), "ko");

        std::vector<int16_t> pcm(kSampleRate, 0);
        CHECK_EQ(tr_pipeline_feed_audio_i16(p, pcm.data(), pcm.size()), 0);
        CHECK_EQ(tr_pipeline_pending_count(p), 0);
        tr_segmenter_config sc;
        tr_segmenter_config_init(&sc);
        CHECK_EQ(sc.end_silence_ms, 700);
        CHECK_EQ(tr_pipeline_set_segmenter_config(p, &sc), 1);
        tr_pipeline_destroy(p);
    }
    tr_string_free(nmtRoot);

    tr_pipeline_config badCfg;
    tr_pipeline_config_init(&badCfg);
    badCfg.whisper_model_path = "/definitely/missing.bin";
    CHECK(tr_pipeline_create(&badCfg) == nullptr);
    CHECK(std::string(tr_last_global_error()).find("not found") != std::string::npos);
}

} // namespace

int main() {
    testSha256();
    testJson();
    testSegmenter();
    testTextUtil();
    testSentenceSplit();
    testLlmEngine();
    testModelManager();
    testPipelineStub();
    testCApi();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
