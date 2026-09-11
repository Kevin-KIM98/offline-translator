# offline-translator

On-device, offline speech interpreter: a C++ engine (RNNoise, whisper.cpp, CTranslate2/Marian,
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
  indicator never clears.
- **Noise.** Recognition needs about 20 dB over the background; below +5 dB whisper invents text.
- **LLM.** Never bias control or end-of-generation tokens in the sampler; generation runs away.
  Small Qwen models drift into Chinese for Thai or Korean targets without the script guard.
- **Several LLMs.** The manifest's `llm` is the default and `llm_options` adds choices (each with a
  `label`); old readers ignore the extras, so never turn `llm` into an array. One LLM is selected at
  a time by id (`llmId` in Kotlin, `--llm-id` in the CLI); Settings → Translation LLM in the app.
  The 3B model is hosted on `models-v1` as `llm_qwen2.5-3b-instruct-q4_k_m.gguf`.
- **Thai.** Only the LLM translates into Thai. The default hands it Marian's English and three
  diverse demonstrations: 26 of 40 test sentences correct against 17 before; Qwen2.5-3B reaches 31.
  Judge a change by reading the output with `python tests/eval/run_th_eval.py`; chrF alone misled.
- **Model selection.** A language choice must download every pair its routes use, including the
  English hops. Before 0.3.7 `ko,ja` got speech recognition only and could not translate.

## Reporting

State what was verified and how. Nothing has been run on a physical device by Claude; do not claim
otherwise. Keep numbers in the docs to ones that were measured.
