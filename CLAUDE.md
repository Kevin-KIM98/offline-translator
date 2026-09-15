# offline-translator

On-device, offline speech interpreter (ko en es vi th ja zh id fr ru): a C++ engine (RNNoise, whisper.cpp, CTranslate2/Marian,
llama.cpp) with a C ABI, and an Android app in `apps/android` that uses it. The user runs the app on
**Android only**; `apps/ios` and the iOS workflows exist but are not in use, so do not spend effort
there unless asked.

The user writes in Korean. Answer in Korean.

## How a change reaches the phone

This repository is the source of truth; the user edits from the Claude mobile app.

1. Commit and push to a branch. Cloud sessions cannot push to `main`.
2. `.github/workflows/android-app.yml` runs for any branch that touches the app or the engine. It
   publishes the APK, replacing the previous one, at
   `https://github.com/Kevin-KIM98/offline-translator/releases/download/dev-build/offline-interpreter-dev.apk`
3. The user opens that link on the phone and installs over the existing app. Models are kept.
4. When the change is good, it goes to `main` through a pull request.

Tell the user the link and the build number once the workflow has finished; a push alone is not
done. If a push changes the engine (`src/`, `include/`, `cmake/`, `CMakeLists.txt`,
`platform/android/`), the workflow builds the engine AAR from that commit, which takes longer.

Never replace `apps/android/app/debug.keystore`. Every APK must be signed with it, or the phone
refuses to update and the user loses the downloaded models (up to 1.7 GB) on reinstall. Its password
is Android's public debug default: fine for development builds, not for a store release.

## Layout

- `src/`, `include/translator/` - engine. `TranslationPipeline` wires denoise, segmentation, STT
  and translation; `ModelManager` selects, verifies and installs models from a manifest.
- `include/translator_c_api.h` - the C ABI the Android JNI layer uses.
- `platform/android/` - Kotlin library (`TranslatorSession`, `ModelRepository`) plus JNI; built as
  the AAR.
- `apps/android/` - the Compose app. UI strings live in `res/values` (English) and `res/values-ko`.
- `tools/translator_cli.cpp` - desktop CLI: `translate`, `speech`, `listen`, `status`, `install`.
- `tests/test_core.cpp` - unit tests. `tests/eval/` - translation quality sets and harness.
- `scripts/` - `fetch_third_party.sh` (engines), `fetch_models.py` (models for desktop testing),
  `prepare_models.py` (publishing models).

## Building and testing in a cloud session

App-only changes (Kotlin, resources): rely on the Android app workflow; a local Android SDK is not
needed. C++ changes: build and run the tests before pushing.

```bash
bash scripts/fetch_third_party.sh --shallow
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTRANSLATOR_BUILD_SHARED=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --target translator_tests translator_cli --parallel 4
./build/translator_tests
```

The full engine build takes several minutes. There is no microphone in the cloud: exercise the
speech path with `translator_cli speech --wav <file> --stream`. Models are not in git; fetch them
with `python scripts/fetch_models.py --models pc-models --langs ko,en`.

## Releases

Engine release: run the Release workflow with a version (`gh workflow run release.yml -f
version=X.Y.Z`). It builds the AAR, updates `Package.swift`, `CMakeLists.txt` and the app's
`engineVersion`, tags and publishes. Afterwards: update the version strings in `README.md`, attach
the APK with `gh workflow run android-app.yml -f release_tag=vX.Y.Z`, and replace the generic
release notes with real ones (`gh release edit vX.Y.Z --notes-file ...`).

Models are hosted on the `models-v1` release and described by `assets/manifest.json`.

## Measured facts to respect

- **Silence.** RNNoise's voice probability fires on room noise and whisper answers noise with a
  fluent invented sentence. `noSpeechThreshold` cannot stop it; the loudness gate in
  `SpeechSegmenter` does (median frame 12 dB over a tracked floor). Any change to segmentation must
  re-check both directions: a quiet room gives zero utterances, and speech 18 dB below normal is
  still transcribed.
- **Push-to-talk.** A flush that queues nothing must call `onNoSpeech`, or the app's progress
  indicator never clears. The callback fires synchronously inside `TranslatorSession.stop()`, so
  the app must switch its indicator on *before* calling stop (2026-09-15: a tap on the talk button
  used to leave "translating" on for good because the callback ran first).
- **Noise.** Whisper must hear the microphone audio, not RNNoise's output (`sttOnDenoisedAudio`
  is off since 0.3.9): the denoiser cost words on clean speech and most of the old "20 dB over the
  background" requirement. RNNoise still supplies the voice activity and the level for the
  loudness gate. Measured with `tests/eval/stt_noise.py` (pink noise into synthesised clips):
  correct down to +5 dB SNR, partial at 0 dB, invented text from about −5 dB. Re-run it after any
  front-end change; clean speech must not lose words and noisy speech must not get worse.
- **Speech recognition per language (0.3.9, synthetic clips).** App-path CER: ko 0.0, en 0.0,
  es 0.1, vi 3.9, th 14.5, ja 2.1, zh 2.7 %; language id 140/140. Beam 5, the per-language prompt
  (zh drifts into traditional characters without it), the adaptive window and microphone audio
  to whisper each earned their place in `tests/eval/run_stt_eval.py`; `auto` detects the language
  first so the prompt applies (`SttEngine::detectLanguage`, `Impl::resolveLang`). Thai is whisper
  small's weak language; only a bigger whisper model helps. Re-run the eval after touching
  SttEngine, the prompts in TextUtil, the segmenter or the denoiser (clips: `stt_synthesize.py`).
- **Conversation mode (0.3.11).** The app's "hands-free" toggle is `TranslatorSession.startConversation(A, B)`:
  `process_pending2(p, "auto", B, A)` picks the direction in the engine (detected B → A, anything
  else → B), the microphone frames are dropped while a translation is spoken and `discard_audio()`
  runs afterwards. Without that mute the phone transcribed its own TTS and answered itself.
  Long speech: the segmenter cuts a non-stop speaker at the longest short pause (≥ 120 ms,
  `splitPauseMs`) in the second half before 15 s; measured with `tests/eval/run_long_eval.py`
  (40 s monologues, pauses shortened to 250 ms): ko 3.3% → 1.4% CER, cuts on clause boundaries.
- **Automatic language detection.** Hands-free mode passes `auto`. Upstream whisper.cpp applies
  `audio_ctx` after language detection, so auto cost 4× a fixed language; `cmake/patches/` moves
  the assignment and `cmake/ThirdParty.cmake` applies it at configure time (auto now ≈ +0.8 s, one
  extra encoder pass). Keep the patch when bumping whisper.cpp; upstream master still has the order.
- **LLM.** Never bias control or end-of-generation tokens in the sampler; generation runs away.
  Small Qwen models drift into Chinese for Thai or Korean targets without the script guard.
- **Several LLMs.** The manifest's `llm` is the default and `llm_options` adds choices (each with a
  `label`); old readers ignore the extras, so never turn `llm` into an array. One LLM is selected at
  a time by id (`llmId` in Kotlin, `--llm-id` in the CLI); Settings → Translation LLM in the app.
  The 3B model is hosted on `models-v1` as `llm_qwen2.5-3b-instruct-q4_k_m.gguf`.
- **App model defaults (0.3.16).** `Prefs.defaultSttId/defaultLlmId`: phones with ≥ 7.5 GB of
  memory default to whisper medium + Qwen2.5 3B (the user's Galaxy S25 has 12 GB), smaller ones
  to small + 1.5B; the settings radios store explicit ids now (null used to mean "manifest
  default", which the app default no longer equals). The manifest's own `stt`/`llm` defaults are
  unchanged for library consumers.
- **Automatic model choice (0.3.17, app only).** `ModelPolicy` picks per conversation pair when
  Settings says "Automatic" (`Prefs.sttAuto/llmAuto`, default on; a tapped model fixes it):
  whisper small unless a language of the pair is in `MEDIUM_LANGS` (th, vi, fr, ja, zh, id) and the
  phone has ≥ 7.5 GB, Qwen 3B only when th is in the pair (1.5B otherwise: no other direction uses
  the LLM). Basis, app-path CER small → medium on the 20-clip sets: ko 0.0/–, en 0.0/–, es 0.1/–,
  ru 0.3/0.4, th 14.5/9.1, vi 3.9/0.8, fr 3.7/0.0, ja 2.1/0.0, zh 2.7/0.4, id 3.5/2.4
  (`run_stt_eval.py --configs app --langs ja,zh,id [--whisper medium]`, 2026-09-15; desktop
  ~1.2 s vs ~2.9 s per clip). `boot()` re-evaluates the policy, so a language change downloads
  and opens the model it calls for.
- **Several speech models (0.3.10).** The manifest's `stt` is the default (whisper small) and
  `stt_options` adds whisper medium (`stt_whisper-medium-q5_0.bin` on `models-v1`, label "Whisper
  medium"); same rules as `llm_options`, never turn `stt` into an array. Selected by id (`sttId` in
  Kotlin, `--stt-id` in the CLI, `tr_mm_status_for_languages_json2` / `tr_mm_stt_model_path_for`);
  Settings → Speech recognition model. Medium measured th 9.1% CER against small's 14.5%, vi 0.8%
  vs 3.9%, ~3× the time. `large-v3-turbo` repeats sentences with the adaptive window: not an option.
- **Thai.** Only the LLM translates into Thai. The default hands it Marian's English and three
  diverse demonstrations: 26 of 40 test sentences correct against 17 before; Qwen2.5-3B reaches 31.
  Judge a change by reading the output with `python tests/eval/run_th_eval.py`; chrF alone misled.
- **LoRA (2026-09-14, not shipped).** QLoRA of Qwen2.5-1.5B on 11k opus-100 pairs (7.5k en→th,
  back-translation filtered by `scripts/filter_parallel.py`) ran in 73 min on the RTX 4050 6 GB
  (`finetune_lora.py train --load-4bit`; prompt/completion form so only the translation is
  scored). Gold-English → Thai improved (chrF 41.3 → 45.4) but the app route did not (39.9 →
  39.4; held-out 28.9 → 28.0) and the model turned casual (ฉัน, no ครับ) from the subtitle data.
  `run_th_eval.py --tuned <gguf>` compares any GGUF. Do not fine-tune on subtitles for an
  interpreter; the Korean→English hop and polite conversational Thai data are the levers.
- **GPU (0.3.14, experimental, unmeasured).** The Android AAR compiles ggml's Vulkan backend
  (`TRANSLATOR_VULKAN=ON`; glslc from the NDK's shader-tools via `GLSLC` in the workflows).
  Whether whisper uses it is `PipelineConfig.useGpu`, off by default in the Kotlin library and
  behind Settings → Performance → "Speech recognition on the GPU" in the app, with a crash-loop
  guard (`gpuTrialPending`: still set at the next launch → switch off + message). The LLM stays on
  the CPU on Android (JNI sets `llm_gpu_layers = 0`), and since 2026-09-15 that is enforced:
  `LlmEngine::load` hands llama.cpp an empty device list when no layers are offloaded. Left to
  its default, llama.cpp b5030 lists every compiled-in GPU device regardless of `n_gpu_layers`,
  initialises the Vulkan backend when the context is created (driver, shader compilation,
  pipeline cache) and its scheduler offloads prompt batches of ≥ 32 tokens to it
  (`ggml_backend_vk_device_offload_op`), so on the phone the LLM — loaded only for pairs that
  need it, i.e. Thai — ran through the Vulkan driver with the GPU setting off. Reported as
  "Thai translation fails, the app closes, a cache error"; not reproduced here (no device), the
  pin is the fix by reading of the llama.cpp source. Nothing about GPU speed or stability has
  been measured on a device; do not claim otherwise.
- **Greetings (2026-09-15).** OPUS-MT ko-en tc-big answers "안녕하세요." with "Good evening." and
  "안녕하십니까" the same (whisper adds the full stop, so the app always hit it); en-ko answers
  a bare "Hello." with the phone greeting "여보세요?". `text::fixedTranslation` in TextUtil holds
  the few bare greetings with a fixed answer, checked per sentence in `NmtEngine::translateDirect`
  before the batch (so the pivot hop of ko → th gets "Hello." too); a greeting inside a longer
  sentence goes to Marian as before ("안녕하세요, 저는 김입니다." → "Hi, I'm Kim."). Measured
  on the desktop with `translator_cli translate --backend marian`.
- **Speed (0.3.13).** `LlmEngine` reuses the KV cache of the shared prompt prefix
  (`cachedPrompt`, `llama_kv_self_seq_rm` from the first differing token): ko→th 2.1 → 1.1 s per
  sentence on the desktop; outputs differ slightly (batch-size rounding), Thai chrF 39.2 → 39.5.
  `SttEngine::detectLanguage` encodes 5 s / 256 positions: −0.1–0.4 s per auto utterance,
  identification unchanged. `TranslatorSession.start` preloads the route(s) (`preloadPair` loads
  every hop). Tried and dropped: whisper beam 3 (th/vi worse for 11% speed), flash attention on
  CPU (no change), `large-v3-turbo` (repeats), Marian beam 2 or 1 (2026-09-15, ko→en tc-big on
  the 40 sentences of the Thai sets against their English references, desktop: beam 4 chrF 60.0
  at 471 ms median, beam 2 59.1 at 416 ms, beam 1 54.7 at 412 ms — the encoder and the call
  cost most of the time, so a narrower beam buys little; Marian time is only cut by batching
  several sentences into one call, as photo translation does). `ko-en` is tc-big since 0.3.13 (manifest 1.6.0,
  entry version 2 → devices re-download it): chrF 59.1 → 62.9 / 44.2 → 53.9 against English
  references, 420 vs 233 ms per sentence; converted from pouta with `vocab_to_yml` (the .vocab
  files are one token per line) + `ctranslate2.converters.marian`, py -3.11.
- **Indonesian (0.3.12).** `id` is the eighth language: OPUS-MT `id-en`/`en-id` on `models-v1`
  (manifest 1.5.0), routes through English, LLM only for id → th. Whisper small on the 20-clip set:
  3.5% CER app path, language id 20/20 (auto 19/20; a three-word clip went to Thai). Adding a
  language touches: `supportedLanguages()` in Types.hpp, `defaultPromptFor`/`hallucinations` in
  TextUtil, `exampleSentence`/`exampleSet`/`scriptAllowed`/the `known` list in LlmEngine,
  `OfflineTranslator.supportedLanguages`, `OfflineTTSManager.localeFor`, the app's `Lang.ALL`,
  `stt_sentences.json` + `NUMBER_WORDS`/`SPACED` in run_stt_eval.py, `finetune_lora.py` tables.
- **French and Russian (0.3.17).** `fr` and `ru` are the ninth and tenth languages: OPUS-MT
  `fr-en`/`en-fr`/`ru-en`/`en-ru` (base, converted with `prepare_models.py nmt` from the Hugging
  Face repos, which are fine for the base models) on `models-v1`, manifest 1.7.0. Both route
  through English; only X → th needs the LLM. `Script::Cyrillic` in LlmEngine: Russian output may
  only use Cyrillic, French only Latin; `dropAppendedSource` also strips trailing English behind
  Russian. Hallucination matching folds Cyrillic case (`toLowerCyrillic`). Checking a converted
  pair from Python needs `</s>` appended to the source tokens (the engine does it itself).
  Whisper app-path CER on the 20-clip sets: small fr 3.7 / ru 0.3 %, medium fr 0.0 / ru 0.4 %,
  language id 40/40 both (`run_stt_eval.py --langs fr,ru`, ~1.2 s vs ~3.7 s per clip on the desktop).
- **Photo translation (0.3.17, app only, unmeasured on a device).** Camera button on the
  conversation screen → `CameraScreen` (CameraX viewfinder or the gallery) → `OcrEngine`
  (tesseract4android-openmp 4.9.0 from JitPack = Tesseract 5.5.1, `PSM_AUTO`, default
  thresholding, longest side scaled to 2000 px) reads positioned words through the result
  iterator, drops words under confidence 30 and lines under 45 (on synthesised photos this cost
  at most 2 points of the words found, 79 → 77%) and groups lines into `OcrRegion`s. Never set
  `thresholding_method=1` (tiled Otsu): PR #2 did, and on photo-like pages Tesseract found 26%
  (ticket) / 21% (price board) of the words against 95% / 79% with the default, so photos stopped
  translating (`tests/eval/ocr_eval.py`, run by the `OCR eval` workflow on Linux, tesseract 5.3.4).
  Orientation: a reading scores confident letters (≥ 70) on lines that run across; under 20 or
  under half of all letters read, the photo is probed turned 0/90/180/270° at 1000 px and turned
  when one scores 1.5× the as-is reading. Lines must run across: Tesseract reads text turned a
  quarter clockwise by itself, as vertical lines the overlay cannot paint, so plain confident
  letters scored sideways like upright. 8/8 synthesised rotations (ticket, board) turned right,
  words found after the turn equal to upright (95% / 77%). Result and failure screens have a
  rotate button (a quarter turn, read again without detection). Within a Tesseract paragraph a line continues the previous
  one only when the heights match, the gap is small and it starts lower-case, follows `-`/`,`,
  both lines are capitals, or the previous line ran to the paragraph's right edge; a line ending
  in a digit or `.!?:;` ends the piece (before, a block's lines were joined into one sentence,
  gluing menu items and sign lines together). Codes, dates, prices and phone numbers
  (`OcrEngine.isTranslatable`: no run of two letters without a digit, or more digits than
  letters) are kept as they are — Marian answered "10SEP26 B0066" and "$ 42.600" with invented
  words. The remaining distinct pieces go to the engine in **one batch**:
  `tr_pipeline_translate_lines` / `TranslatorSession.translateLines` (0.3.17; one piece per
  line, one output line per input line, `NmtEngine::translateDirect` keeps lines apart through
  every hop and sends all their sentences to CTranslate2 together). Desktop, 40 lines of the
  price board es→ko: 18.6 s one call each → 6.8 s in one batch (`translator_cli translate
  --lines --batch`); es→en in Python 6.3 → 2.4 s, identical output. An app running on an engine
  AAR without the call (app-only push on main uses the released AAR) hits UnsatisfiedLinkError,
  caught in `translateImage`, and falls back to one call per piece — cut an engine release to
  make the batch the default. `TranslatedPhoto` paints each translation over the photo: colour
  sampled around the box, the largest font that fits, pinch/double-tap zoom, tap to uncover the
  original, "Original" toggle; the result footer shows OCR and translation seconds and the piece
  count; the `Turn` (`fromImage`) joins the regions with newlines, and a long press on it in the
  conversation copies the text read as well as the translation (to report a wrong reading). All-caps text of cased languages is
  sentence-cased first (`OcrEngine.forTranslation`): 50 sign texts through the app's pairs came
  out right 16 in capitals, 36 sentence-cased (en→ko 12→14/20, fr 1→7, es 0→8, ru 3→7 of 10;
  `tests/eval/ocr_caps_eval.py`, judged by reading). Language files are Tesseract `tessdata_fast`
  (`ocr_<code>.traineddata` on `models-v1`, listed in the manifest's `ocr` array with the ISO
  code; `prepare_models.py manifest` emits it from `<root>/ocr`) and live in `files/ocr/tessdata`,
  downloaded by `OcrModels` on first use (sha256-checked), separate from the engine's store. The
  engine and older apps ignore the `ocr` key. The text must be in one of the two conversation
  languages; the user picks which. Settings → "Download everything" fetches the OCR files with
  the models (2026-09-15: `Phase.Setup.ocrPending`, installed after the engine models). A picked
  gallery photo is opened off the main thread by `PhotoFiles` (ImageDecoder, then BitmapFactory
  with the EXIF rotation when that fails) and a failure shows the decoder's message; the camera
  screen and its language side are `rememberSaveable`, so the picker's result still lands when
  the activity was recreated behind it.
- **Model selection.** A language choice must download every pair its routes use, including the
  English hops. Before 0.3.7 `ko,ja` got speech recognition only and could not translate.

## Reporting

State what was verified and how. Nothing has been run on a physical device by Claude; do not claim
otherwise. Keep numbers in the docs to ones that were measured.
