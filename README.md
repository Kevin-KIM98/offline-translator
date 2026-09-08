# Offline 5-language speech translator — native engine

On-device, no-network speech translation for iOS and Android:

```
mic 16 kHz ─► RNNoise ─► VAD segmenter ─► whisper.cpp ─► CTranslate2 (OPUS-MT INT8) ─► OS TTS
              denoise     utterances       STT (ko/en/ja/zh/es)   NMT + pivot routing   AVSpeech / android.tts
```

The C++ core is a single shared library with a C ABI; Kotlin and Swift layers wrap it and add
microphone capture, model download and TTS. The original specification is preserved in
[docs/SPEC.md](docs/SPEC.md).

## Layout

```
include/translator/        C++ core headers
  TranslationPipeline.hpp  RNNoise → segmenter → whisper → NMT orchestration
  ModelManager.hpp         manifest parsing, SHA-256 verification, atomic install
  NmtEngine.hpp            CTranslate2 + SentencePiece, direct/pivot routing
  SttEngine.hpp            whisper.cpp wrapper
  Denoiser.hpp             RNNoise wrapper (+ energy-VAD fallback)
  SpeechSegmenter.hpp      VAD hysteresis → utterances
  Sha256.hpp, MiniJson.hpp, FileUtil.hpp   dependency-free utilities
include/translator_c_api.h C ABI used by JNI / Objective-C++
src/                       implementations
platform/android/          Gradle library module: JNI bridge + Kotlin (NativeBridge, OfflineTranslator,
                           ModelRepository, AudioCapture, OfflineTTSManager, TranslatorSession)
platform/ios/              Objective-C++ wrapper + Swift (ModelDownloader, AudioCapture,
                           OfflineTTSManager, TranslatorSession)
tools/translator_cli.cpp   desktop CLI (status / verify / install / transcribe / translate / speech)
tests/test_core.cpp        unit tests (no models needed)
scripts/                   fetch_third_party.{sh,ps1}, build_android.sh, build_ios.sh, prepare_models.py
assets/manifest.example.json
docs/                      SPEC.md (original), API.md (C API + JSON schemas), MODELS.md (asset prep)
```

## Build

### 1. Fetch engines

```bash
scripts/fetch_third_party.sh          # macOS / Linux / Git Bash
```
```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_third_party.ps1   # Windows
```

Pinned: RNNoise v0.2, whisper.cpp v1.7.5, CTranslate2 v4.5.0 (with submodules), SentencePiece v0.2.0.
Each engine is optional — if a directory is missing the core builds that stage as a stub, which
is how the unit tests run on a bare machine.

### 2. Desktop (tests + CLI)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/translator_tests                       # 198 checks
./build/translator_cli speech --models ~/models --wav sample.wav --tgt en --stream
```

### 3. Android

Add the module to your app:

```kotlin
// settings.gradle.kts
include(":offline-translator")
project(":offline-translator").projectDir = file("../translator/platform/android")
```

Gradle drives CMake (`platform/android/CMakeLists.txt` → root project). Only `arm64-v8a` is
enabled by default; `-DTRANSLATOR_VULKAN=ON` adds GPU offload for whisper on Adreno/Mali.
`scripts/build_android.sh` produces a standalone `.so` for non-Gradle setups.

### 4. iOS

```bash
scripts/build_ios.sh     # → dist/ios/OfflineTranslatorCore.xcframework (device + simulator)
```

Add the xcframework, `platform/ios/OfflineTranslator/*.{h,mm,swift}` and the bridging header
to the app target. CoreML (Neural Engine encoder) and Metal are on by default.

## Usage

### Android

```kotlin
val repo = ModelRepository(context, manifestUrl = "https://assets.yourdomain.com/models/manifest.json")
repo.refreshManifest()
val needed = repo.statusForLanguages(listOf("ko", "en")).filter { it.needsDownload }
repo.installAll(needed).collect { event -> /* progress UI */ }

val session = TranslatorSession(context, repo.pipelineConfig())
session.onResult = { r -> transcript.add("${r.sourceText} → ${r.translatedText}") }
session.start(sourceLang = "auto", targetLang = "en")   // speaks results via TTS
```

### iOS

```swift
let downloader = ModelDownloader(manifestURL: URL(string: "https://assets.yourdomain.com/models/manifest.json"))
await downloader.refreshManifest()
for await event in downloader.installAll(downloader.status(languages: ["ko", "en"])) { /* progress UI */ }

let session = try TranslatorSession(config: downloader.manager.pipelineConfig())
session.onResult = { r in print(r.sourceText, "→", r.translatedText) }
try session.start(sourceLang: "auto", targetLang: "en")
```

### C / C++

See [docs/API.md](docs/API.md). Minimal:

```c
tr_pipeline_config cfg; tr_pipeline_config_init(&cfg);
cfg.whisper_model_path = ".../models/stt/whisper-small-q5_1.bin";
cfg.nmt_root_dir = ".../models/nmt";
tr_pipeline* p = tr_pipeline_create(&cfg);
char* json = tr_pipeline_process_speech(p, pcm, n, "auto", "en");   // TranslationResult JSON
tr_string_free(json); tr_pipeline_destroy(p);
```

## Model assets

`scripts/prepare_models.py` downloads whisper ggml models, converts OPUS-MT pairs to
CTranslate2 INT8 and writes `manifest.json` with per-file SHA-256. Details, sizes and the
pivot strategy (8 X↔en pairs cover all 20 directions) are in [docs/MODELS.md](docs/MODELS.md).

Devices verify every file before it is moved into `models/`, record what was installed, and
re-download only entries whose manifest version/hash changed (`update_available`).

## Deviations from the original spec (and why)

| Spec | Implemented | Reason |
|---|---|---|
| `GGML_USE_COREML` / `GGML_USE_NNAPI` defines, hand-listed ggml sources | `add_subdirectory(whisper.cpp)` with `WHISPER_COREML` / `GGML_METAL` / `GGML_VULKAN` options | Those defines are not how whisper.cpp enables CoreML; ggml has no NNAPI backend. Using the upstream CMake keeps working across whisper.cpp releases. |
| NMT input `{" " + text}` as one token | SentencePiece tokenization per sentence, batch decode, `▁` detokenization | CTranslate2 expects subword tokens; a single raw-text token produces `<unk>`. |
| One `Translator` for one `nmt_model_dir` | `NmtEngine` with lazy per-pair loading + pivot routing | Five languages need up to 20 directions; direct X↔ko models don't all exist. |
| `rnnoise_process_frame` on −1..1 floats | Scaled to 16-bit range and back | RNNoise expects PCM16-range floats; feeding normalized audio silently disables denoising. |
| Zip per NMT pair with one SHA-256 | Files mode (per-file hashes) preferred, zip mode still accepted | Per-file hashes allow resumable downloads and deep verification of installed files; no unzip dependency on iOS. |
| Download logic inside C++ ModelManager | C++ verifies/installs; URLSession / HttpURLConnection download | Background transfers, TLS and cellular policies belong to the OS layer; the core stays portable and testable. |
| Whole-buffer STT only | Streaming VAD segmenter + one-shot API | Continuous conversation mode needs utterance boundaries; one-shot remains for push-to-talk. |

## Status

Verified on Windows 11 / MSVC 2022 with all four engines compiled in (`/W4`, zero warnings in
project code):

| check | result |
|---|---|
| `translator_tests` (stub build, no engines) | 198 / 198 |
| `translator_tests` (full build, real engines) | 188 / 188 |
| whisper `tiny-q5_1`, `samples/jfk.wav` → text | 557 ms |
| OPUS-MT ko-en INT8, 3 Korean sentences → English | 523 ms |
| Korean TTS WAV → whisper → NMT (one-shot) | 1.33 s |
| same file, streaming (RNNoise + VAD → 3 utterances) | 0.7–0.8 s each |

Kotlin/Swift layers are written against the C API and still need an app project and a device
build (`scripts/build_android.sh`, `scripts/build_ios.sh`).

### Build notes

* **RNNoise v0.2** references Opus' `os_support.h` (not shipped); `cmake/shim/rnnoise/` supplies
  it. `rnnoise_data_little.c` is excluded (duplicate symbols) — `-DTRANSLATOR_RNNOISE_LITTLE=ON`
  swaps in the smaller model.
* **CTranslate2 on Windows** forces the static CRT; `cmake/ThirdParty.cmake` re-targets its
  objects to `/MD`. Its replica worker keeps a `thread_local ruy::Context` whose destructor joins
  threads under the loader lock, which deadlocks on `Translator` destruction when intra-op threads
  > 1 — the engine therefore caps CTranslate2 to one thread **on Windows only** (INT8 Marian is
  ~50 ms/sentence single-threaded). Android/iOS use `n_threads`.
* CTranslate2 is built with the compiler's OpenMP (`OPENMP_RUNTIME=COMP`) except on iOS.
* Third-party engines always build static; the shipped artifact is a single `offline_translator`
  library.
* No Python/torch is needed for a first test: several pre-converted CTranslate2 OPUS-MT models
  exist on Hugging Face (e.g. `manancode/opus-mt-ko-en-ctranslate2-android`, 80 MB INT8) — drop
  the five files into `models/nmt/ko-en/`.
