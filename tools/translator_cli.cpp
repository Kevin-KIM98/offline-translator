// Desktop CLI for exercising the engine without a phone.
//
//   translator_cli status    --models <dir> [--manifest <file|url-json>] [--deep]
//   translator_cli verify    --file <path> --sha256 <hex> [--size N]
//   translator_cli install   --models <dir> --id <model-id> --staged <path> [--no-verify]
//   translator_cli sha256    <file>
//   translator_cli transcribe --models <dir> --wav <file> [--lang auto]
//   translator_cli translate --models <dir> --text "..." --src ko --tgt en
//   translator_cli speech    --models <dir> --wav <file> [--src auto] --tgt en [--stream] [--no-denoise]
//
// WAV input: PCM16 or float32, mono/stereo, any rate (resampled to 16 kHz).
#include "translator/FileUtil.hpp"
#include "translator/MiniJson.hpp"
#include "translator/ModelManager.hpp"
#include "translator/Sha256.hpp"
#include "translator/TranslationPipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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
        "  verify     --file <path> --sha256 <hex> [--size N]\n"
        "  install    --models <dir> --id <model-id> --staged <path> [--no-verify]\n"
        "  sha256     <file>\n"
        "  transcribe --models <dir> --wav <file> [--lang auto] [--threads N]\n"
        "  translate  --models <dir> --text \"...\" --src ko --tgt en\n"
        "  speech     --models <dir> --wav <file> [--src auto] --tgt en [--stream] [--no-denoise]\n",
        stderr);
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

int cmdStatus(const Args& a) {
    if (!a.has("models")) { usage(); return 2; }
    ModelManager mm(a.get("models"));
    if (!loadManager(a, mm)) return 1;
    const bool deep = a.has("deep");
    std::printf("%s\n", mm.statusJson(deep).dump(2).c_str());
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
        else if (loadManager(a, mm)) cfg.whisperModelPath = mm.sttModelPath();
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
    cfg.beamSize = std::atoi(a.get("beam", "2").c_str());
    if (a.has("prompt")) cfg.initialPrompt = a.get("prompt");

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
    if (!a.has("models") || !a.has("text") || !a.has("src") || !a.has("tgt")) { usage(); return 2; }
    TranslationPipeline p;
    if (!makePipeline(a, p, false)) return 1;
    const TranslationResult r = p.translateText(a.get("text"), a.get("src"), a.get("tgt"));
    std::printf("%s\n", r.toJson().dump(2).c_str());
    return r.ok ? 0 : 1;
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
    usage();
    return 2;
}
