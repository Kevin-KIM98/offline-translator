# Offline 5-language speech translator

On-device, no-network speech translation for **Android and iOS** (Korean · English · Japanese · Chinese · Spanish):

```
mic 16 kHz ─► RNNoise ─► VAD segmenter ─► whisper.cpp ─► CTranslate2 (OPUS-MT INT8) ─► OS TTS
              denoise     utterances       STT (5 langs)    NMT + pivot routing        AVSpeech / android.tts
```

Everything runs inside one native library with a C ABI; thin Kotlin and Swift layers add the
microphone, model download and TTS. Models (≈ 0.2 GB STT + 80 MB per language pair) are fetched on
first launch from this repo's GitHub releases — no server of your own is needed.

## Install in an app

### Android (2 lines)

1. Download `offline-translator-<version>.aar` from the [latest release](https://github.com/Kevin-KIM98/offline-translator/releases) into `app/libs/`.
2. `app/build.gradle.kts`:

```kotlin
implementation(files("libs/offline-translator-0.2.0.aar"))
implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
```

Add `<uses-permission android:name="android.permission.RECORD_AUDIO" />` (INTERNET is declared by the library). `minSdk 26`, `arm64-v8a`.

```kotlin
// e.g. in a ViewModel / lifecycleScope
val session = TranslatorSession.prepare(context, languages = listOf("ko", "en")) { done, total ->
    progressBar.progress = (done * 100 / total).toInt()     // first launch only: downloads models
}
session.onResult = { r -> transcript.add("${r.sourceText}  →  ${r.translatedText}") }
session.start(sourceLang = "ko", targetLang = "en")         // speaks the translation via TTS
// ... session.stop(); session.close()
```

### iOS (Swift Package)

Xcode → *File → Add Package Dependencies…* → `https://github.com/Kevin-KIM98/offline-translator` → add **OfflineTranslator**.
Add `NSMicrophoneUsageDescription` to Info.plist. iOS 15+.

```swift
import OfflineTranslator

let session = try await TranslatorSession.prepare(languages: ["ko", "en"]) { done, total in
    progress = Double(done) / Double(total)                  // first launch only
}
session.onResult = { r in transcript.append("\(r.sourceText)  →  \(r.translatedText)") }
try session.start(sourceLang: "ko", targetLang: "en")
```

Both `prepare` calls are safe on every launch: models already on the device are verified against
the manifest and skipped. Text-only translation is `session.translate(...)`.

### Self-hosting models

Point `manifestUrl` at your own copy of `assets/manifest.json` (regenerate it with
`scripts/prepare_models.py manifest`). See [docs/MODELS.md](docs/MODELS.md).

## What you get

| stage | engine | notes |
|---|---|---|
| noise suppression + VAD | RNNoise | 30 ms frames, utterance segmentation with pre-roll / hangover |
| speech recognition | whisper.cpp `small-q5_1` (190 MB) | beam 5, punctuated per-language prompts, conversation context, hallucination filter, adaptive encoder window |
| translation | CTranslate2 OPUS-MT INT8 (≈ 80 MB / pair) | beam 4, repetition control, sentence batching, English-pivot routing for pairs without a direct model, per-language post-processing |
| speech output | AVSpeechSynthesizer / android.speech.tts | offline OS voices |

Measured on a desktop CPU (no GPU), Korean speech → English, whisper `small`:

| | before quality pass | after |
|---|---|---|
| transcript | no punctuation → 3 sentences merged, "Hello" dropped by NMT | `안녕하세요. 오늘 날씨가 정말 좋네요. 내일 회의는 오후 3시에 시작합니다.` |
| translation | "It's a great day. Tomorrow's meeting starts at 3:00 p.m." | "Hello. It's a great day. Tomorrow's meeting starts at 3:00 p.m." |
| latency per utterance (streaming) | 3.7 – 4.1 s | 1.15 – 1.34 s |

Even the 32 MB `tiny` model now produces a correct, punctuated transcript for this sample.

Text translation samples with the shipped models (desktop CPU, beam 4):

| direction | input | output |
|---|---|---|
| ko→en | 안녕하세요. 오늘 날씨가 정말 좋네요. 내일 회의는 오후 세 시에 시작합니다. | Hello. It's a great day. Tomorrow's meeting starts at three o'clock in the afternoon. |
| en→ko | Hello. Tomorrow's meeting starts at 3 p.m. This product has a long battery life and a bright screen, so it is good for outdoor use. | 안녕하세요. 내일의 회의는 오후 3시에 시작됩니다. 이 제품은 긴 배터리 수명과 밝은 화면을 가지고 있으므로 야외 사용에 좋습니다. |
| ja→en | こんにちは。今日はいい天気ですね。明日の会議は午後3時に始まります。 | Hello. It's a nice day, isn't it? Tomorrow's meeting begins at 3 p.m. |
| en→ja | Hello. It's a great day. Is this product good for outdoor use? | こんにちは。いい天気だ。この製品は屋外で使うのに良いでしょうか。 |
| ja→ko (pivot via en) | こんにちは。明日の会議は午後3時に始まります。一番近い駅はどこですか？ | 안녕하세요. 내일 모임은 오후 3시에 시작합니다. 가장 가까운 역은 어디인가요? |
| es→ko (pivot via en) | Hola. ¿Dónde está la estación de metro más cercana? | 안녕하세요. 가장 가까운 지하철 역은 어디죠? |

Models are hosted on the [`models-v1` release](https://github.com/Kevin-KIM98/offline-translator/releases/tag/models-v1)
and described by [assets/manifest.json](assets/manifest.json) (per-file SHA-256). A first launch with
Korean + English downloads about 500 MB; each additional language adds roughly 160 MB.

## Repository layout

```
include/translator/        C++ core headers (TranslationPipeline, ModelManager, NmtEngine, SttEngine,
                           Denoiser, SpeechSegmenter, TextUtil, Sha256, MiniJson, FileUtil)
include/translator_c_api.h C ABI used by JNI / Objective-C++
src/                       implementations
platform/android/          Gradle library module (JNI + Kotlin API) — builds the AAR
platform/ios/              Objective-C++ wrapper + Swift API — targets of Package.swift
Package.swift              Swift Package (binary XCFramework + wrappers)
tools/translator_cli.cpp   desktop CLI (status / verify / install / transcribe / translate / speech)
tests/test_core.cpp        unit tests (no models needed)
scripts/                   fetch_third_party.{sh,ps1}, build_android.sh, build_ios.sh, prepare_models.py
assets/manifest.json       default model manifest (GitHub-release hosted assets)
docs/                      SPEC.md (original spec), API.md (C API + JSON), MODELS.md (asset prep)
.github/workflows/         release.yml — builds AAR + XCFramework + CLI and publishes a release
```

## Building from source

```bash
scripts/fetch_third_party.sh --shallow     # RNNoise, whisper.cpp, CTranslate2, SentencePiece (pinned)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTRANSLATOR_STRICT_DEPS=ON
cmake --build build --config Release --parallel 8
./build/Release/translator_tests           # 219 checks
```

Desktop CLI examples (models under `models/`, see [docs/MODELS.md](docs/MODELS.md) for a no-Python setup):

```bash
./build/Release/translator_cli translate --models models --text "안녕하세요. 오늘 날씨가 좋네요." --src ko --tgt en
./build/Release/translator_cli speech --models models --whisper models/stt/whisper-small-q5_1.bin --wav input.wav --src ko --tgt en --stream
```

Mobile artifacts are produced by the **Release** workflow (Actions → Release → version). It builds the
AAR on Ubuntu, the XCFramework on macOS, rewrites `Package.swift` with the new checksum, tags
`v<version>` and attaches everything to the release. `scripts/build_android.sh` /
`scripts/build_ios.sh` do the same locally.

## Deviations from the original spec (and why)

| Spec | Implemented | Reason |
|---|---|---|
| `GGML_USE_COREML` / `GGML_USE_NNAPI` defines, hand-listed ggml sources | `add_subdirectory(whisper.cpp)` with `WHISPER_COREML` / `GGML_METAL` / `GGML_VULKAN` options | Those defines are not how whisper.cpp enables CoreML; ggml has no NNAPI backend. |
| NMT input `{" " + text}` as one token | SentencePiece tokenization per sentence, EOS handling from `config.json`, batch decode | CTranslate2 expects subword tokens; raw text yields `<unk>`. |
| One `Translator` for one `nmt_model_dir` | `NmtEngine` with lazy per-pair loading + pivot routing | 5 languages need 20 directions; direct X↔ko models don't all exist. |
| `rnnoise_process_frame` on −1..1 floats | Scaled to 16-bit range and back | RNNoise expects PCM16-range floats. |
| Zip per NMT pair with one SHA-256 | Files mode (per-file hashes) preferred, zip mode still accepted | Resumable downloads and deep verification without an unzip dependency. |
| Download logic inside C++ | C++ verifies/installs; URLSession / HttpURLConnection download | TLS, background transfers and cellular policy belong to the OS layer. |
| Whole-buffer STT only | Streaming VAD segmenter + one-shot API | Conversation mode needs utterance boundaries. |

## Build notes

* **RNNoise v0.2** references Opus' `os_support.h` (not shipped); `cmake/shim/rnnoise/` supplies it.
  `rnnoise_data_little.c` is excluded (duplicate symbols); `-DTRANSLATOR_RNNOISE_LITTLE=ON` swaps it in.
* **CTranslate2 on Windows** forces the static CRT; `cmake/ThirdParty.cmake` re-targets it to `/MD`.
  Its replica worker's thread-local ruy context deadlocks on destruction with intra-op threads > 1
  under the Windows loader lock, so CTranslate2 is capped to one thread on Windows only.
* CTranslate2 uses the compiler's OpenMP (`OPENMP_RUNTIME=COMP`) except on iOS.
* All third-party engines build static; apps ship a single library.

## Status

* C++ core + all four engines verified on Windows/MSVC 2022: 219/219 unit tests, Korean speech → English end to end (one-shot and streaming).
* Android AAR / iOS XCFramework are built by CI; the Kotlin/Swift layers compile against the C API but have not yet been exercised on a physical device from this workstation — please report device issues.
