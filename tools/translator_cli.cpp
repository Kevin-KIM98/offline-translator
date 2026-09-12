// Desktop CLI for exercising the engine without a phone.
//
//   translator_cli status    --models <dir> [--manifest <file|url-json>] [--deep] [--langs ko,en]
//   translator_cli verify    --file <path> --sha256 <hex> [--size N]
//   translator_cli install   --models <dir> --id <model-id> --staged <path> [--no-verify]
//   translator_cli sha256    <file>
//   translator_cli transcribe --models <dir> --wav <file> [--lang auto]
//   translator_cli translate --models <dir> --text "..." --src ko --tgt en
//   translator_cli speech    --models <dir> --wav <file> [--src auto] --tgt en [--stream] [--no-denoise]
//   translator_cli listen    --models <dir> [--src auto] --tgt en [--device N] [--seconds N]
//
// WAV input: PCM16 or float32, mono/stereo, any rate (resampled to 16 kHz).
#include "translator/FileUtil.hpp"
#include "translator/MiniJson.hpp"
#include "translator/ModelManager.hpp"
#include "translator/Sha256.hpp"
#include "translator/TranslationPipeline.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if TRANSLATOR_HAS_MINIAUDIO
// Capture only: the rest of miniaudio (decoding, playback graph, engine) would triple the
// compile time of this one file.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#endif

using namespace translator;

namespace {

struct Args {
    std::string command;
    std::map<std::string, std::string> opts;
    std::vector<std::string> positional;

    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

Args parseArgs(int argc, char** argv) {
    Args a;
    if (argc > 1) a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            const std::string key = s.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a.opts[key] = argv[++i];
            else a.opts[key] = "1";
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

void usage() {
    std::fputs(
        "translator_cli <command> [options]\n"
        "  status     --models <dir> [--manifest <file>] [--deep]\n"
        "             [--langs ko,en] [--llm if-needed|always|never] [--llm-id <id>] [--stt-id <id>]\n"
        "             only what those languages need; --stt-id picks a whisper model from stt_options\n"
        "  verify     --file <path> --sha256 <hex> [--size N]\n"
        "  install    --models <dir> --id <model-id> --staged <path> [--no-verify]\n"
        "  sha256     <file>\n"
        "  transcribe --models <dir> --wav <file> [--lang auto] [--threads N] [--stt-id <id> | --whisper <file>]\n"
        "  translate  --models <dir> --text \"...\" --src ko --tgt en [--llm <gguf> | --llm-id <id>] [--backend auto|marian|llm]\n"
        "             [--llm-examples none|single|diverse] [--no-llm-pivot] [--lines]  --lines: one sentence per stdin line\n"
        "  speech     --models <dir> --wav <file> [--src auto] --tgt en [--stream] [--no-denoise] [--no-llm]\n"
        "             [--stt-denoised]  whisper hears RNNoise's output instead of the microphone audio\n"
        "  listen     --models <dir> [--src auto] --tgt en [--device N] [--seconds N] [--list-devices]\n"
        "             [--record <out.wav>] keeps what the microphone heard, for replay\n"
        "             live microphone; speak, pause, and each utterance is translated\n",
        stderr);
}

bool writeWav(const std::string& path, const std::vector<float>& pcm, std::string& err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    const std::uint32_t rate = 16000, dataBytes = static_cast<std::uint32_t>(pcm.size() * 2);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    out.write("data", 4); u32(dataBytes);
    for (const float f : pcm) {
        const float clamped = f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped * 32767.0f)));
    }
    return out.good();
}

// ---------------------------------------------------------------------------
// WAV reader
// ---------------------------------------------------------------------------

bool readWav(const std::string& path, std::vector<float>& out16k, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    auto u16 = [&](std::size_t o) { return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[o]) | (static_cast<unsigned char>(bytes[o + 1]) << 8)); };
    auto u32 = [&](std::size_t o) { return static_cast<std::uint32_t>(u16(o)) | (static_cast<std::uint32_t>(u16(o + 2)) << 16); };

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::size_t dataOff = 0, dataLen = 0;
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::string id(bytes.data() + pos, 4);
        const std::uint32_t len = u32(pos + 4);
        const std::size_t body = pos + 8;
        if (id == "fmt ") {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && len >= 26) format = u16(body + 24); // WAVE_FORMAT_EXTENSIBLE
        } else if (id == "data") {
            dataOff = body;
            dataLen = std::min<std::size_t>(len, bytes.size() - body);
            break;
        }
        pos = body + len + (len & 1);
    }
    if (!dataOff || !channels || !rate) {
        err = "malformed WAV (missing fmt/data)";
        return false;
    }

    std::vector<float> mono;
    const std::size_t frameBytes = static_cast<std::size_t>(channels) * bits / 8;
    const std::size_t frames = dataLen / frameBytes;
    mono.reserve(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const std::size_t o = dataOff + f * frameBytes + static_cast<std::size_t>(c) * bits / 8;
            float v = 0.0f;
            if (format == 1 && bits == 16) v = static_cast<float>(static_cast<std::int16_t>(u16(o))) / 32768.0f;
            else if (format == 1 && bits == 32) v = static_cast<float>(static_cast<std::int32_t>(u32(o))) / 2147483648.0f;
            else if (format == 1 && bits == 8) v = (static_cast<float>(static_cast<unsigned char>(bytes[o])) - 128.0f) / 128.0f;
            else if (format == 3 && bits == 32) { std::uint32_t u = u32(o); std::memcpy(&v, &u, 4); }
            else { err = "unsupported WAV format (need PCM 8/16/32 or float32)"; return false; }
            sum += v;
        }
        mono.push_back(sum / static_cast<float>(channels));
    }

    if (rate == static_cast<std::uint32_t>(kSampleRate)) {
        out16k = std::move(mono);
        return true;
    }
    // Linear resample (fine for speech; use a proper resampler in production apps).
    const double ratio = static_cast<double>(rate) / kSampleRate;
    const std::size_t n = static_cast<std::size_t>(mono.size() / ratio);
    out16k.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double src = i * ratio;
        const std::size_t k = static_cast<std::size_t>(src);
        const double frac = src - k;
        const float a = mono[std::min(k, mono.size() - 1)], b = mono[std::min(k + 1, mono.size() - 1)];
        out16k[i] = static_cast<float>(a + (b - a) * frac);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool loadManager(const Args& a, ModelManager& mm) {
    std::string err;
    if (a.has("manifest")) {
        if (!mm.loadManifestFile(a.get("manifest"), &err)) {
            std::fprintf(stderr, "manifest error: %s\n", err.c_str());
            return false;
        }
        mm.saveManifest();
    } else if (!mm.loadCachedManifest(&err)) {
        std::fprintf(stderr, "no manifest: pass --manifest <file> (or place manifest.json in the models dir)\n");
        return false;
    }
    return true;
}

std::vector<std::string> splitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : csv) {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int cmdStatus(const Args& a) {
    if (!a.has("models")) { usage(); return 2; }
    ModelManager mm(a.get("models"));
    if (!loadManager(a, mm)) return 1;
    const bool deep = a.has("deep");

    if (!a.has("langs")) {
        std::printf("%s\n", mm.statusJson(deep).dump(2).c_str());
        return 0;
    }

    // Same selection the phone apps make: only what these languages need, plus the LLM when
    // some direction has no Marian route.
    const std::string mode = a.get("llm", "if-needed");
    const ModelManager::LlmMode llmMode = mode == "always" ? ModelManager::LlmMode::Always
                                        : mode == "never"  ? ModelManager::LlmMode::Never
                                                           : ModelManager::LlmMode::IfNeeded;
    Json j = Json::object();
    j.set("manifest_version", mm.manifestVersion());
    j.set("models_root", a.get("models"));
    Json arr = Json::array();
    std::uint64_t pending = 0;
    for (const auto& st : mm.statusForLanguages(splitCsv(a.get("langs")), deep, llmMode, a.get("llm-id", ""), a.get("stt-id", ""))) {
        if (st.needsDownload()) pending += st.entry.totalBytes();
        arr.push_back(st.toJson());
    }
    j.set("pending_bytes", pending);
    j.set("models", arr);
    std::printf("%s\n", j.dump(2).c_str());
    return 0;
}

int cmdVerify(const Args& a) {
    if (!a.has("file")) { usage(); return 2; }
    ModelManager mm(".");
    std::uint64_t lastPct = 100;
    const VerifyResult r = mm.verifyFile(a.get("file"), a.get("sha256"), std::strtoull(a.get("size", "0").c_str(), nullptr, 10),
        [&](std::uint64_t done, std::uint64_t total) {
            const std::uint64_t pct = total ? done * 100 / total : 100;
            if (pct != lastPct) { std::fprintf(stderr, "\rverifying %3llu%%", static_cast<unsigned long long>(pct)); lastPct = pct; }
            return true;
        });
    std::fprintf(stderr, "\n");
    std::printf("%s\n", verifyResultName(r));
    return r == VerifyResult::Ok ? 0 : 1;
}

int cmdInstall(const Args& a) {
    if (!a.has("models") || !a.has("id") || !a.has("staged")) { usage(); return 2; }
    ModelManager mm(a.get("models"));
    if (!loadManager(a, mm)) return 1;
    std::string err;
    if (!mm.install(a.get("id"), a.get("staged"), !a.has("no-verify"), nullptr, &err)) {
        std::fprintf(stderr, "install failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("installed %s\n", a.get("id").c_str());
    return 0;
}

int cmdSha256(const Args& a) {
    if (a.positional.empty()) { usage(); return 2; }
    for (const auto& f : a.positional) {
        const std::string h = Sha256::hashFile(f);
        if (h.empty()) { std::fprintf(stderr, "cannot read %s\n", f.c_str()); return 1; }
        std::printf("%s  %s\n", h.c_str(), f.c_str());
    }
    return 0;
}

bool makePipeline(const Args& a, TranslationPipeline& p, bool needWhisper) {
    ModelManager mm(a.get("models"));
    PipelineConfig cfg;
    cfg.nmtRootDir = mm.nmtRootDir();
    if (needWhisper) {
        if (a.has("whisper")) cfg.whisperModelPath = a.get("whisper");
        else if (loadManager(a, mm)) cfg.whisperModelPath = mm.sttModelPath(a.get("stt-id", ""));
        else {
            // Fall back to the first .bin in <models>/stt
            for (const auto& f : fs::listDirectory(mm.sttDir()))
                if (f.size() > 4 && f.substr(f.size() - 4) == ".bin") { cfg.whisperModelPath = fs::join(mm.sttDir(), f); break; }
        }
        if (cfg.whisperModelPath.empty()) {
            std::fprintf(stderr, "no whisper model: pass --whisper <file> or install one via the manifest\n");
            return false;
        }
    }
    cfg.nThreads = std::atoi(a.get("threads", "4").c_str());
    cfg.useGpu = !a.has("cpu");
    cfg.enableDenoise = !a.has("no-denoise");
    cfg.sttOnDenoisedAudio = a.has("stt-denoised");   // pre-0.3.9 behaviour, for comparison
    if (a.has("beam")) cfg.beamSize = std::atoi(a.get("beam").c_str());
    if (a.has("stt-beam")) cfg.sttBeamSize = std::atoi(a.get("stt-beam").c_str());
    if (a.has("no-speech")) cfg.noSpeechThreshold = static_cast<float>(std::atof(a.get("no-speech").c_str()));
    if (a.has("no-context")) cfg.useContextPrompt = false;
    if (a.has("full-audio-ctx")) cfg.sttAdaptiveAudioContext = false;
    if (a.has("no-default-prompt")) cfg.useDefaultPrompts = false;
    if (a.has("prompt")) cfg.initialPrompt = a.get("prompt");
    if (a.has("llm")) cfg.llmModelPath = a.get("llm");
    else if (loadManager(a, mm)) cfg.llmModelPath = mm.llmModelPath(a.get("llm-id", ""));
    if (!cfg.llmModelPath.empty() && !fs::isFile(cfg.llmModelPath)) cfg.llmModelPath.clear();
    if (a.has("no-llm")) cfg.llmModelPath.clear();   // skip the 1 GB load when only Marian is needed
    const std::string backend = a.get("backend", "auto");
    cfg.backend = backend == "llm" ? TranslationBackend::Llm : backend == "marian" ? TranslationBackend::Marian : TranslationBackend::Auto;
    if (a.has("llm-ctx")) cfg.llmContextSize = std::atoi(a.get("llm-ctx").c_str());
    if (a.has("llm-examples")) {
        const std::string e = a.get("llm-examples");
        cfg.llmExamples = e == "none" ? LlmExamples::None : e == "single" ? LlmExamples::Single : LlmExamples::Diverse;
    }
    if (a.has("no-llm-pivot")) cfg.llmPivotThroughEnglish = false;

    std::string err;
    if (!p.initialize(cfg, &err)) {
        std::fprintf(stderr, "init failed: %s\n", err.c_str());
        return false;
    }
    std::fprintf(stderr, "capabilities: %s\n", p.capabilities().dump().c_str());
    return true;
}

int cmdTranscribe(const Args& a) {
    if (!a.has("models") || !a.has("wav")) { usage(); return 2; }
    std::vector<float> pcm;
    std::string err;
    if (!readWav(a.get("wav"), pcm, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    TranslationPipeline p;
    if (!makePipeline(a, p, true)) return 1;
    const SttResult r = p.transcribe(pcm.data(), pcm.size(), a.get("lang", "auto"));
    if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    std::printf("[%s] %s\n(%.0f ms)\n", r.detectedLang.c_str(), r.text.c_str(), r.elapsedMs);
    return 0;
}

int cmdTranslate(const Args& a) {
    const bool lines = a.has("lines");
    if (!a.has("models") || (!lines && !a.has("text")) || !a.has("src") || !a.has("tgt")) { usage(); return 2; }
    TranslationPipeline p;
    if (!makePipeline(a, p, false)) return 1;
    if (!lines) {
        const TranslationResult r = p.translateText(a.get("text"), a.get("src"), a.get("tgt"));
        std::printf("%s\n", r.toJson().dump(2).c_str());
        return r.ok ? 0 : 1;
    }
    // One sentence per stdin line, one compact JSON result per stdout line. The models load once,
    // which is what makes comparing configurations over a test set practical.
    std::string line;
    int failures = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const TranslationResult r = p.translateText(line, a.get("src"), a.get("tgt"));
        if (!r.ok) ++failures;
        std::printf("%s\n", r.toJson().dump().c_str());
        std::fflush(stdout);
    }
    return failures == 0 ? 0 : 1;
}

int cmdSpeech(const Args& a) {
    if (!a.has("models") || !a.has("wav") || !a.has("tgt")) { usage(); return 2; }
    std::vector<float> pcm;
    std::string err;
    if (!readWav(a.get("wav"), pcm, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    TranslationPipeline p;
    if (!makePipeline(a, p, true)) return 1;
    const std::string src = a.get("src", "auto"), tgt = a.get("tgt");

    if (!a.has("stream")) {
        const TranslationResult r = p.processSpeechToTranslation(pcm.data(), pcm.size(), src, tgt);
        std::printf("%s\n", r.toJson().dump(2).c_str());
        return r.ok ? 0 : 1;
    }

    // Streaming mode: simulate a microphone delivering 20 ms chunks through the segmenter.
    int utterances = 0;
    const std::size_t chunk = 320;
    for (std::size_t i = 0; i < pcm.size(); i += chunk) {
        const std::size_t n = std::min(chunk, pcm.size() - i);
        if (p.feedAudio(pcm.data() + i, n)) {
            while (p.hasPendingUtterance()) {
                const TranslationResult r = p.processPendingUtterance(src, tgt);
                std::printf("--- utterance %d ---\n%s\n", ++utterances, r.toJson().dump(2).c_str());
            }
        }
    }
    if (p.flushAudio()) {
        while (p.hasPendingUtterance()) {
            const TranslationResult r = p.processPendingUtterance(src, tgt);
            std::printf("--- utterance %d (flushed) ---\n%s\n", ++utterances, r.toJson().dump(2).c_str());
        }
    }
    std::fprintf(stderr, "%d utterance(s)\n", utterances);
    return 0;
}

// ---------------------------------------------------------------------------
// listen: live microphone → denoise → VAD → STT → translation
// ---------------------------------------------------------------------------

#if TRANSLATOR_HAS_MINIAUDIO

struct CaptureBuffer {
    std::mutex mutex;
    std::vector<float> samples;
    std::atomic<float> level{0.0f};
};

void captureCallback(ma_device* device, void* /*output*/, const void* input, ma_uint32 frameCount) {
    auto* buffer = static_cast<CaptureBuffer*>(device->pUserData);
    const float* in = static_cast<const float*>(input);
    if (!buffer || !in || frameCount == 0) return;

    double sum = 0.0;
    for (ma_uint32 i = 0; i < frameCount; ++i) sum += static_cast<double>(in[i]) * in[i];
    buffer->level.store(static_cast<float>(std::sqrt(sum / frameCount)), std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(buffer->mutex);
    buffer->samples.insert(buffer->samples.end(), in, in + frameCount);
}

bool stderrIsTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

void drawMeter(float rms, int utterances, bool busy) {
    // Carriage returns only make sense on a terminal; piped output would collect one line
    // per redraw.
    static const bool tty = stderrIsTerminal();
    if (!tty) return;
    // -50 dBFS .. 0 dBFS across 20 cells.
    const double db = 20.0 * std::log10(std::max(rms, 1e-6f));
    const int cells = static_cast<int>(std::max(0.0, std::min(1.0, (db + 50.0) / 50.0)) * 20.0);
    std::string bar(20, '.');
    for (int i = 0; i < cells; ++i) bar[static_cast<std::size_t>(i)] = '#';
    std::fprintf(stderr, "\r  mic [%s]  utterances: %d  %s   ", bar.c_str(), utterances,
                 busy ? "(translating)" : "(Enter to stop)");
    std::fflush(stderr);
}

int cmdListen(const Args& a) {
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        std::fprintf(stderr, "cannot initialise the audio backend\n");
        return 1;
    }

    ma_device_info* captureInfos = nullptr;
    ma_uint32 captureCount = 0;
    ma_context_get_devices(&context, nullptr, nullptr, &captureInfos, &captureCount);

    if (a.has("list-devices")) {
        std::printf("capture devices:\n");
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            std::printf("  %u: %s%s\n", i, captureInfos[i].name, captureInfos[i].isDefault ? "  (default)" : "");
        }
        ma_context_uninit(&context);
        return 0;
    }

    if (!a.has("models") || !a.has("tgt")) {
        ma_context_uninit(&context);
        usage();
        return 2;
    }

    TranslationPipeline pipeline;
    if (!makePipeline(a, pipeline, true)) {
        ma_context_uninit(&context);
        return 1;
    }

    const std::string src = a.get("src", "auto");
    const std::string tgt = a.get("tgt");
    const int seconds = std::atoi(a.get("seconds", "0").c_str());

    CaptureBuffer buffer;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = 16000;
    config.dataCallback = captureCallback;
    config.pUserData = &buffer;
    if (a.has("device")) {
        const int index = std::atoi(a.get("device").c_str());
        if (index < 0 || static_cast<ma_uint32>(index) >= captureCount) {
            std::fprintf(stderr, "no capture device %d (see --list-devices)\n", index);
            ma_context_uninit(&context);
            return 2;
        }
        config.capture.pDeviceID = &captureInfos[index].id;
    }

    ma_device device;
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
        std::fprintf(stderr, "cannot open the microphone\n");
        ma_context_uninit(&context);
        return 1;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        std::fprintf(stderr, "cannot start capture\n");
        ma_device_uninit(&device);
        ma_context_uninit(&context);
        return 1;
    }

    std::fprintf(stderr, "listening on \"%s\" at %u Hz — %s to %s\n", device.capture.name,
                 device.sampleRate, src.c_str(), tgt.c_str());
    std::fprintf(stderr, "speak, then pause; each utterance is translated. Enter stops.\n");

    std::atomic<bool> stop{false};
    std::thread waiter([&stop]() {
        // Only a real line stops the loop; an immediate EOF (stdin redirected from /dev/null,
        // or a non-interactive run) must not look like the user pressed Enter.
        std::string line;
        if (std::getline(std::cin, line)) stop.store(true);
    });
    waiter.detach();

    const auto started = std::chrono::steady_clock::now();
    auto lastDraw = started;
    std::vector<float> chunk;
    std::vector<float> recording;
    const std::string recordPath = a.get("record", "");
    int utterances = 0;

    auto drain = [&](const char* label) {
        while (pipeline.hasPendingUtterance()) {
            drawMeter(buffer.level.load(std::memory_order_relaxed), utterances, true);
            const TranslationResult r = pipeline.processPendingUtterance(src, tgt);
            if (r.ok && r.sourceText.empty()) continue;  // silence
            ++utterances;
            if (stderrIsTerminal()) std::fprintf(stderr, "\r%60s\r", "");
            if (!r.ok) {
                std::printf("[%d] error: %s\n", utterances, r.error.c_str());
                continue;
            }
            std::string route;
            for (const auto& hop : r.route) route += (route.empty() ? "  via " : "->") + hop;
            std::printf("[%d%s] %s (%s)\n     -> %s (%s)  %.0f ms%s\n", utterances, label,
                        r.sourceText.c_str(), r.sourceLang.c_str(),
                        r.translatedText.c_str(), r.targetLang.c_str(), r.totalMs, route.c_str());
            std::fflush(stdout);
        }
    };

    while (!stop.load()) {
        {
            std::lock_guard<std::mutex> lock(buffer.mutex);
            chunk.swap(buffer.samples);
            buffer.samples.clear();
        }
        if (!chunk.empty()) {
            if (!recordPath.empty()) recording.insert(recording.end(), chunk.begin(), chunk.end());
            if (pipeline.feedAudio(chunk.data(), chunk.size())) drain("");
            chunk.clear();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastDraw > std::chrono::milliseconds(120)) {
            lastDraw = now;
            drawMeter(buffer.level.load(std::memory_order_relaxed), utterances, false);
        }
        if (seconds > 0 && now - started >= std::chrono::seconds(seconds)) break;
    }

    ma_device_stop(&device);
    {
        std::lock_guard<std::mutex> lock(buffer.mutex);
        if (!buffer.samples.empty()) pipeline.feedAudio(buffer.samples.data(), buffer.samples.size());
        buffer.samples.clear();
    }
    if (pipeline.flushAudio()) drain(" flushed");
    if (stderrIsTerminal()) std::fprintf(stderr, "\r%60s\r", "");
    std::fprintf(stderr, "%d utterance(s)\n", utterances);
    if (!recordPath.empty()) {
        std::string werr;
        if (writeWav(recordPath, recording, werr)) {
            std::fprintf(stderr, "wrote %s (%.1f s)\n", recordPath.c_str(),
                         static_cast<double>(recording.size()) / 16000.0);
        } else {
            std::fprintf(stderr, "%s\n", werr.c_str());
        }
    }

    ma_device_uninit(&device);
    ma_context_uninit(&context);
    return 0;
}

#else

int cmdListen(const Args&) {
    std::fprintf(stderr,
                 "this build has no microphone support: third_party/miniaudio/miniaudio.h was\n"
                 "missing at configure time. Run scripts/fetch_third_party.sh and rebuild.\n");
    return 2;
}

#endif

} // namespace

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Windows hands main() the ANSI code page (e.g. CP949); re-read the command line as
    // UTF-16 and convert to UTF-8 so Korean/Japanese/Chinese text survives. Also make the
    // console print UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Ptrs;
    if (wargv) {
        for (int i = 0; i < wargc; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], len, nullptr, nullptr);
            utf8Args.push_back(std::move(s));
        }
        LocalFree(wargv);
        for (auto& s : utf8Args) utf8Ptrs.push_back(&s[0]);
        argc = static_cast<int>(utf8Ptrs.size());
        argv = utf8Ptrs.data();
    }
#endif
    const Args a = parseArgs(argc, argv);
    if (a.command == "status") return cmdStatus(a);
    if (a.command == "verify") return cmdVerify(a);
    if (a.command == "install") return cmdInstall(a);
    if (a.command == "sha256") return cmdSha256(a);
    if (a.command == "transcribe") return cmdTranscribe(a);
    if (a.command == "translate") return cmdTranslate(a);
    if (a.command == "speech") return cmdSpeech(a);
    if (a.command == "listen") return cmdListen(a);
    usage();
    return 2;
}
