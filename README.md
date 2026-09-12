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
[offline-interpreter-1.0.0.apk](https://github.com/Kevin-KIM98/offline-translator/releases/download/v0.3.10/offline-interpreter-1.0.0.apk)
for an arm64 Android 8+ device, or build either app from source:

```
cd apps/android && ./gradlew assembleDebug          # no NDK needed
```

The newest build of any branch is always at
[offline-interpreter-dev.apk](https://github.com/Kevin-KIM98/offline-translator/releases/download/dev-build/offline-interpreter-dev.apk).
From development build 14 on, every APK is signed with the same project key and numbered higher
than the one before, so it installs as an update and keeps the downloaded models. An app installed
from an earlier build has to be uninstalled once before the first of these.

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
implementation(files("libs/offline-translator-0.3.10.aar"))
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

`languages` decides what gets downloaded: every direction between the chosen languages gets the
models its route uses.

| `languages` | download | how it translates |
|---|---|---|
| `["ko","en"]` | ≈ 0.5 GB | two dedicated Korean–English models |
| `["ko","ja"]` | ≈ 0.66 GB | four dedicated models, both directions through English |
| `["ko","th"]` | ≈ 1.7 GB | Thai→Korean through English; Korean→Thai by the LLM, which is handed Marian's English |

Up to 0.3.6 a choice like `["ko","ja"]` selected no English models and so downloaded speech
recognition alone; nothing could translate. Pass `backend = LLM` (Kotlin
`TranslationBackend.LLM`, Swift `.LLM`) to translate everything with the LLM — slower, but a single
model for all 42 directions with automatic source-language detection.

### Self-hosting models

Point `manifestUrl` at your own copy of `assets/manifest.json` (regenerate it with
`scripts/prepare_models.py manifest`). See [docs/MODELS.md](docs/MODELS.md).

## What you get

| stage | engine | notes |
|---|---|---|
| noise suppression + VAD | RNNoise | 30 ms frames: voice activity and the level for the loudness gate; utterance segmentation with pre-roll / hangover. Whisper hears the microphone audio, not RNNoise's output (0.3.9) |
| speech recognition | whisper.cpp `small-q5_1` (190 MB); `medium-q5_0` (539 MB) selectable in Settings (0.3.10) | beam 5, punctuated per-language prompts, conversation context, hallucination filter, adaptive encoder window (also for automatic language detection, 0.3.9) |
| translation (Marian) | CTranslate2 OPUS-MT INT8 (≈ 80 MB / pair, 11 pairs) | beam 4, repetition control, sentence batching, English-pivot routing, per-language post-processing |
| translation (LLM) | llama.cpp + Qwen2.5-1.5B-Instruct Q4_K_M (1.1 GB) | directions no dedicated model covers, starting from Marian's English where a pair reaches it; three demonstrations, script guard, greedy decoding, output cleanup; any→any with source auto-detect when used on its own; `scripts/finetune_lora.py` adapts it to your domain |
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

### How much background noise it tolerates

Measured by mixing pink noise, which is close to the spectrum of a crowd or a PA system, into
speech at a known signal-to-noise ratio and reading what came out. Two measurements, because the
front end changed in 0.3.9.

Before 0.3.9 whisper heard RNNoise's output. A real microphone recording, Korean:

| SNR | result |
|---|---|
| +20 dB | correct, wording occasionally varies |
| +15 dB | words break up, one sentence lost, translation misleading |
| +10 dB | meaning gone ("지하철역" heard as "대학설력") |
| +5 dB | **invents text**: returns the recognition prompt instead of what was said |
| 0 dB | invents text only |

Since 0.3.9 whisper hears the microphone audio; RNNoise still supplies the voice activity and the
level for the loudness gate. Synthesised clips, same noise (`python tests/eval/stt_noise.py`,
ko/en/ja/es "let's meet in the hotel lobby at three tomorrow afternoon"):

| SNR | whisper hears the microphone (0.3.9) | whisper hears RNNoise's output (before) |
|---|---|---|
| clean | all four correct | ko: 호텔 로비 → 호텔러비 |
| +20 dB | all four correct | ko: one word off |
| +15 dB | all four correct | ko: 3시 → 1시, en: "tomorrow afternoon" lost |
| +10 dB | all four correct | ko: meaning gone, en: truncated, ja: one word off |
| +5 dB | ko/en/es correct, ja: one clause replaced | ko/ja: replaced by stock phrases |
| 0 dB | ko: one word off, en/ja: truncated | ko: prompt fragments ("안녕하세요" ×3) |
| −5 dB | ko: **invents** the prompt sentence, ja: nothing | invented |

RNNoise's output was costing whisper words even on clean speech, and most of the old 20 dB
requirement was the denoiser, not whisper. With the microphone path the practical requirement is
roughly **5–10 dB of headroom over the background** on these clips, and the invented-text failure
starts around 0 dB. The real-microphone measurement is kept above because it is the only one made
with a real recording; expect a real room to sit between the two columns. A quiet room or an
office is comfortable; a busy restaurant or a street is workable; a bar or a club, where the music
alone is 90 dB and a speaker at arm's length arrives 15–25 dB below it, is still out of reach.

The failure at the bottom is the dangerous kind: whisper does not return nothing, it returns a
confident sentence that was never said. Nothing downstream can tell the difference.

What actually helps, in order of effect: a close-talking microphone (a headset boom 3 cm from the
mouth buys 20–25 dB over a phone at arm's length), a directional or beamforming capture mode,
and a denoiser built for non-stationary noise — RNNoise is tuned for steady noise like fans and
hum and does little against music or overlapping voices.

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
./build/Release/translator_tests           # 279 checks
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
* **whisper.cpp is patched at configure time** (`cmake/patches/whisper.cpp-audio-ctx-before-lang-detect.patch`,
  applied by `cmake/ThirdParty.cmake`, a no-op once applied). Upstream `whisper_full()` sets the
  reduced encoder window (`audio_ctx`) only after automatic language detection, so `language = "auto"`
  first ran a full 30 s encoder pass: 4.4–4.8 s per utterance against 1.1–1.3 s with a fixed language
  on this desktop CPU. With the assignment moved in front of the detection, a detection pass costs
  one encoder run at the reduced window; the pipeline runs it itself (`SttEngine::detectLanguage`)
  and then transcribes with the detected language's prompt: 1.9–2.5 s median per utterance.
* All third-party engines build static; apps ship a single library.

## Status

* C++ core + all five engines (RNNoise, whisper.cpp, CTranslate2, SentencePiece, llama.cpp) verified on Windows/MSVC 2022: 279/279 unit tests, Korean speech → English end to end (one-shot and streaming), LLM translation across all seven languages.
* "Training": the LLM is a pre-trained multilingual model configured by prompting; `scripts/finetune_lora.py` is the supervised fine-tuning path (LoRA → merged GGUF) for domain data — it requires a GPU and was not run as part of this repo.
* Android AAR / iOS XCFramework are built by CI; the Kotlin/Swift layers compile against the C API but have not yet been exercised on a physical device from this workstation — please report device issues.
* **Silence no longer produces invented sentences.** RNNoise reports a high voice probability
  for room noise, and whisper answers a noise-only segment with a fluent sentence, usually a
  fragment of the language's default prompt. The segmenter now also requires an utterance's
  median frame to sit at least 12 dB above the noise floor it tracks between utterances
  (`speechAboveNoiseDb`), with an absolute backstop at -60 dBFS. Measured on a quiet desk
  microphone: noise windows reach 6.7 dB above the floor, speech at least 18.9 dB. A live
  40 s run that previously produced four invented sentences now produces none, and speech
  attenuated by 18 dB is still transcribed.
* **Speech recognition checked per language (0.3.9).** `tests/eval/stt_synthesize.py` makes 140
  clips (10 travel-conversation sentences × 7 languages × 2 neural voices, a second of silence
  around each) and `tests/eval/run_stt_eval.py` runs them through the app's streaming path and
  five one-shot variants. Through the app path, desktop CPU, whisper `small-q5_1`:

  | | ko | en | es | vi | th | ja | zh |
  |---|---|---|---|---|---|---|---|
  | character error rate | 0.0% | 0.0% | 0.1% | 3.9% | 14.5% | 2.1% | 2.7% |
  | exact transcripts | 20/20 | 20/20 | 19/20 | 10/20 | 2/20 | 13/20 | 14/20 |
  | median STT time | 1.16 s | 1.15 s | 1.23 s | 1.36 s | 1.48 s | 1.23 s | 1.22 s |

  Language identification with `auto`: 140/140. The comparison also settled four questions.
  Whisper on RNNoise's output instead of the microphone audio: ko 11.4%, zh 9.1%, th 19.4%,
  vi 6.4% — so whisper now hears the microphone (see the noise section). Without the per-language
  prompt: zh 7.8%, drifting into traditional characters — and `auto` used to decode without a
  prompt because the language was not known yet, so the pipeline now runs whisper's detector
  first and prompts for the detected language: `auto` now matches the fixed-language numbers
  (zh 1.2%, 140/140 identified) at 1.9–2.5 s per utterance against 1.2–1.5 s, the detection
  pass being the difference. Greedy instead of beam 5: vi 4.9%,
  th 16.1%, only 12% faster — beam kept. Whisper's full 30 s window instead of the adaptive one:
  no gain, 3.3× slower — adaptive kept. Thai is whisper `small`'s weak language (tone marks and
  vowel spellings: ล็อบบี้ → รอบบี); a larger whisper model is the lever, at about four times
  the size. Synthetic voices are cleaner than a phone microphone, so these are upper bounds.
* **Whisper medium selectable (0.3.10).** Settings → Speech recognition model offers `medium-q5_0`
  (539 MB) next to the default `small`; the app downloads it when chosen, and the manifest carries it
  as `stt_options` the way `llm_options` carries the 3B LLM. On the same 140 clips, one-shot, desktop
  CPU: th 14.5% → 9.1% CER (exact transcripts 2 → 5 of 20), vi 3.9% → 0.8%, ja 2.1% → 0.2%,
  zh 2.7% → 0.0%, ko/en/es unchanged at 0; 3.4–4.4 s per utterance against 1.2–1.5 s. Thai's
  remaining errors are vowel and tone spellings of single words (ตั๋ว → ตัว), not lost sentences.
  `large-v3-turbo` was tried and dropped: with the adaptive encoder window it repeated sentences
  from the first Korean clips on. `translator_cli ... --stt-id whisper-medium-q5_0` and
  `scripts/fetch_models.py --stt-id ...` select it on the desktop.
