# Offline 7-language speech translator

On-device, no-network speech translation for **Android and iOS** — Korean · English · Spanish ·
Vietnamese · Thai · Japanese · Chinese, in every direction:

```
mic 16 kHz ─► RNNoise ─► VAD segmenter ─► whisper.cpp ─► translation ─────────────────► OS TTS
              denoise     utterances       STT + language   Marian INT8 pair when one    AVSpeech /
                                           auto-detect      exists, else on-device LLM    android.tts
                                                            (llama.cpp, Qwen2.5-Instruct)
```

Two translation engines share one pipeline. **Marian** (CTranslate2 OPUS-MT, ≈ 80 MB per pair) is
fast and strong for X↔English; the **LLM** (Qwen2.5-1.5B-Instruct Q4, 1.1 GB, via llama.cpp)
translates any pair in one hop and can detect the source language itself. The default `auto`
backend uses Marian wherever a pair exists (directly or through English) and the LLM for the rest,
so the LLM is only downloaded when the chosen languages need it.

Everything runs inside one native library with a C ABI; thin Kotlin and Swift layers add the
microphone, model download and TTS. Models are fetched on first launch from this repo's GitHub
releases — no server of your own is needed.

## Try it: the app

`apps/` holds a finished two-way interpreter for both platforms — first-run model download,
push-to-talk per speaker, a replayable transcript, hands-free mode, keyboard input and model
management. Grab
[offline-interpreter-1.0.0.apk](https://github.com/Kevin-KIM98/offline-translator/releases/download/v0.3.4/offline-interpreter-1.0.0.apk)
for an arm64 Android 8+ device, or build either app from source:

```
cd apps/android && ./gradlew assembleDebug          # no NDK needed
```

See [apps/android](apps/android/README.md) and [apps/ios](apps/ios/README.md).

## Test on a desktop first

The whole pipeline runs on Windows, macOS and Linux, so recognition and translation quality can
be judged before any phone is involved. Build the CLI, fetch the models a language pair needs,
then talk into the machine's microphone:

```bash
scripts/fetch_third_party.sh --shallow
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target translator_cli

python scripts/fetch_models.py --models pc-models --langs ko,en
./build/Release/translator_cli listen --models pc-models --src ko --tgt en
```

`listen` shows a level meter, and prints each utterance as soon as you stop speaking:

```
[1] 가장 가까운 지하철역이 어디인가요? (ko)
     -> Where's the nearest subway station? (en)  1420 ms  via ko-en
```

`fetch_models.py` picks exactly the models the phone would for those languages — including the
LLM when a direction has no dedicated model — and hands verification and installation to the
same C++ code the apps use, so a directory it produces is what a device would end up with.
Downloads resume; re-run it after an interruption. `--dry-run` lists sizes without downloading.

Useful flags: `--list-devices` and `--device N` to pick a microphone, `--seconds N` for an
unattended run, `--backend llm` to force the LLM, `--src auto` to let whisper detect the
language. Without a microphone, `translator_cli speech --wav <file> --tgt en --stream` runs the
same path over a recording.

Text-to-speech is not part of this: it comes from the phone OS, so it can only be judged on a
device.

## Install in your own app

### Android (2 lines)

1. Download `offline-translator-<version>.aar` from the [latest release](https://github.com/Kevin-KIM98/offline-translator/releases) into `app/libs/`.
2. `app/build.gradle.kts`:

```kotlin
implementation(files("libs/offline-translator-0.3.4.aar"))
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

`languages` decides what gets downloaded: `["ko","en"]` ≈ 0.5 GB (STT + two Marian pairs);
`["ko","th"]` adds the LLM (1.1 GB) because no Marian model translates into Thai. Pass
`backend = LLM` (Kotlin `TranslationBackend.LLM`, Swift `.LLM`) to translate everything with the
LLM — slower, but a single model for all 42 directions with automatic source-language detection.

### Self-hosting models

Point `manifestUrl` at your own copy of `assets/manifest.json` (regenerate it with
`scripts/prepare_models.py manifest`). See [docs/MODELS.md](docs/MODELS.md).

## What you get

| stage | engine | notes |
|---|---|---|
| noise suppression + VAD | RNNoise | 30 ms frames, utterance segmentation with pre-roll / hangover |
| speech recognition | whisper.cpp `small-q5_1` (190 MB) | beam 5, punctuated per-language prompts, conversation context, hallucination filter, adaptive encoder window |
| translation (Marian) | CTranslate2 OPUS-MT INT8 (≈ 80 MB / pair, 11 pairs) | beam 4, repetition control, sentence batching, English-pivot routing, per-language post-processing |
| translation (LLM) | llama.cpp + Qwen2.5-1.5B-Instruct Q4_K_M (1.1 GB) | any→any in one hop, source auto-detect, chat-template prompting, greedy decoding, output cleanup; `scripts/finetune_lora.py` adapts it to your domain |
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

LLM samples (Qwen2.5-1.5B, desktop CPU, 1.3–3.7 s each): ko→vi "Xin chào. Hội nghị của tôi sẽ bắt đầu vào 3 giờ chiều ngày mai.",
ko→th "สวัสดีครับ/ค่ะ วันพรุ่งนี้การประชุมจะเริ่มเวลา 3 โมง", auto→en (Spanish in) "Hello. Where is the nearest metro station?".
Into Korean the Marian pivot (th→en→ko: "안녕하세요, 내일 3시에 미팅 시작해요 가장 가까운 지하철역은 어디인가요?") beats the 1.5B LLM,
which is why `auto` prefers Marian.

Models are hosted on the [`models-v1` release](https://github.com/Kevin-KIM98/offline-translator/releases/tag/models-v1)
and described by [assets/manifest.json](assets/manifest.json) (per-file SHA-256): whisper `small-q5_1`,
11 Marian pairs (ko/ja/zh/es/vi/th ↔ en, en→ko tc-big) and the Qwen2.5-1.5B GGUF.

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

* C++ core + all five engines (RNNoise, whisper.cpp, CTranslate2, SentencePiece, llama.cpp) verified on Windows/MSVC 2022: 260/260 unit tests, Korean speech → English end to end (one-shot and streaming), LLM translation across all seven languages.
* "Training": the LLM is a pre-trained multilingual model configured by prompting; `scripts/finetune_lora.py` is the supervised fine-tuning path (LoRA → merged GGUF) for domain data — it requires a GPU and was not run as part of this repo.
* Android AAR / iOS XCFramework are built by CI; the Kotlin/Swift layers compile against the C API but have not yet been exercised on a physical device from this workstation — please report device issues.
* **Known issue — invented sentences in silence.** With no one speaking, the segmenter still
  hands room noise to whisper, which returns a fluent sentence. Measured on a quiet desktop:
  every result was a fragment of the language's default prompt ("안녕하세요. 오늘 회의는 오후 3시에
  시작합니다.") or a stock subtitle phrase. Lowering `noSpeechThreshold` to 0.3 does not help, because
  whisper reports these as confident speech, and `--no-default-prompt` only changes what it invents.
  The fix belongs in the voice-activity gate, which should not emit an utterance at that energy in
  the first place. Reproduce with `translator_cli listen --models <dir> --src ko --tgt en --seconds 12`
  in a quiet room.
