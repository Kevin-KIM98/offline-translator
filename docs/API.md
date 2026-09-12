# C API reference (`include/translator_c_api.h`)

The C ABI is the single integration point for Android (JNI) and iOS (Objective-C++).
All strings are UTF-8. `char*` return values are heap-allocated → `tr_string_free()`.
`int` returns: `1` success, `0` failure (read `*_last_error`), negative = I/O error / aborted.

## Threading

* A `tr_pipeline` may be used from several threads. Audio feeding (`tr_pipeline_feed_audio*`,
  `denoise_frame`, `flush`, `reset`) takes an audio-side lock and is cheap enough for the
  capture thread. Inference calls (`process_*`, `transcribe`, `translate_text`) take a
  separate engine lock and block for the duration (hundreds of ms) — call them off-main.
* `tr_model_manager` calls are serialized, except `tr_mm_verify_file`, which hashes without
  holding the lock so a UI thread can still query status during a long verification.

## Pipeline

```c
tr_pipeline_config cfg; tr_pipeline_config_init(&cfg);
cfg.whisper_model_path = "/…/models/stt/whisper-small-q5_1.bin";
cfg.nmt_root_dir       = "/…/models/nmt";
cfg.n_threads = 4; cfg.use_gpu = 1; cfg.enable_denoise = 1; cfg.pivot_langs = "ko,en";
tr_pipeline* p = tr_pipeline_create(&cfg);        // NULL → tr_last_global_error()
```

### Quality knobs (`tr_pipeline_config`, 0.2+)

| field | default | effect |
|---|---|---|
| `stt_beam_size` | 5 | whisper beam search width; 1 = greedy (fastest) |
| `use_default_prompts` | 1 | punctuated per-language prompt → whisper emits punctuation, so NMT sees real sentences |
| `use_context_prompt` | 1 | previous transcript (≤ 200 chars) appended to the prompt for consistent vocabulary |
| `stt_adaptive_audio_ctx` | 1 | shrink whisper's 30 s encoder window to the utterance length (~3× faster on CPU) |
| `stt_on_denoised_audio` | 0 | 0.3.9+: give whisper RNNoise's output instead of the microphone audio. RNNoise always supplies the voice activity and the level for the loudness gate; whisper is trained on noisy speech and the denoiser's artifacts cost words on clean speech |
| `no_speech_threshold` | 0.85 | drop whisper segments it flags as non-speech |
| `clean_transcripts` | 1 | remove `[music]`-style markers, runaway repetitions and stock hallucinations ("시청해주셔서 감사합니다") |
| `beam_size` | 4 | NMT beam |
| `no_repeat_ngram_size` | 3 | NMT n-gram blocking |
| `repetition_penalty` | 1.1 | NMT |
| `post_process_translations` | 1 | capitalization / spacing / full-width punctuation per target language |
| `pivot_langs` | `en,ko` | pivot order when no direct pair exists (X→en→Y models are the strongest) |

### LLM backend (0.3+)

| field | default | effect |
|---|---|---|
| `llm_model_path` | NULL | GGUF instruction model (Qwen2.5-Instruct). NULL → Marian only |
| `translation_backend` | 0 | 0 **auto**: Marian pair when one exists (direct or via English), else the LLM, which is handed Marian's English when a pair reaches English (0.3.7+) · 1 Marian only · 2 LLM only |
| `llm_context_size` | 1024 | prompt + output tokens |
| `llm_max_output_tokens` | 256 | hard cap (also bounded by input length) |
| `llm_temperature` | 0 | 0 = greedy (recommended) |
| `llm_gpu_layers` | 99 | Metal / Vulkan offload when compiled in |
| `llm_system_prompt` | NULL | override the built-in interpreter instruction |

The C++ `PipelineConfig` has two more LLM settings that the C ABI leaves at their defaults:
`llmExamples` (`Diverse`, three unrelated demonstrations; `Single` is the 0.3.6 behaviour, whose one
travel question leaked into similar sentences; `None`) and `llmPivotThroughEnglish` (`true`).

With the LLM, `source_lang` may be `"auto"` for text translation too: the model is told to detect
the language. `route` in the result is `["llm"]` when the LLM translated the original text, and
`["ko-en", "llm"]` when a Marian hop produced the English it translated from.
`tr_build_capabilities()` reports `"llama": true` when llama.cpp is compiled in;
`tr_pipeline_capabilities()` adds `"llm_loaded"` and `"backend"`.

`tr_mm_status_for_languages_json(m, "ko,th", deep, llm_mode)` returns the models the requested
directions route through: direct pairs, both hops of a route through English, and the
source-to-English pair ahead of an LLM direction. Up to 0.3.6 only pairs whose two languages were
both requested were returned, so `"ko,ja"` came back with speech recognition alone. `llm_mode` 0
includes the manifest's `llm` entry only when some requested direction has no Marian route,
1 always, 2 never. `tr_mm_llm_model_path(m)` gives the install path.

A manifest may offer several LLMs: `llm` (the default) plus an `llm_options` array, each with a
`label` that status JSON passes through. `tr_mm_status_for_languages_llm_json(m, langs, deep,
llm_mode, llm_id)` and `tr_mm_llm_model_path_for(m, llm_id)` select one; `""` or an unknown id
means the default, and only the selected LLM appears in the list. (0.3.8)

Several speech models work the same way: `stt` (the default, whisper small) plus an `stt_options`
array with a `label` each. `tr_mm_status_for_languages_json2(m, langs, deep, llm_mode, llm_id,
stt_id)` and `tr_mm_stt_model_path_for(m, stt_id)` select one; only the selected speech model
appears in the list, and `""` or an unknown id means the default. (0.3.10)

### Streaming (microphone)

```
feed_audio(pcm, n)  → 1 when an utterance is complete
   ├─ RNNoise 480-sample frames → VAD probability
   └─ SpeechSegmenter: start after 3 voiced frames, end after 700 ms silence,
      300 ms pre-roll, 400 ms min, 15 s max (tr_pipeline_set_segmenter_config)
process_pending(src, tgt) → TranslationResult JSON (pops the oldest utterance)
flush_audio()             → force-close (push-to-talk release)
```

### One-shot

| function | returns |
|---|---|
| `tr_pipeline_transcribe(p, pcm, n, "auto")` | `{"ok":true,"text":"…","detected_lang":"ko","elapsed_ms":412.3}` |
| `tr_pipeline_translate_text(p, text, "ko", "en")` | TranslationResult |
| `tr_pipeline_process_speech(p, pcm, n, "auto", "en")` | TranslationResult |

### TranslationResult JSON

```json
{
  "ok": true,
  "error": "…",                      // only when ok=false
  "source_text": "안녕하세요",
  "source_lang": "ko",               // detected when "auto" was requested
  "target_lang": "en",
  "translated_text": "Hello.",
  "route": ["ko-en"],                // ["ja-ko","ko-en"] when pivoting
  "timings": { "stt_ms": 380.1, "nmt_ms": 61.2, "total_ms": 442.0 }
}
```

`ok=true` with an empty `source_text` means the audio contained no speech — not an error.

### Language routing

`translate(src → tgt)` uses the direct pair directory `<nmt>/<src>-<tgt>` when present,
otherwise the first pivot in `pivot_langs` for which both `<src>-<pivot>` and `<pivot>-<tgt>`
exist. `tr_pipeline_can_translate` answers this without loading anything. Pairs are loaded
lazily on first use and stay resident; `tr_pipeline_unload_pair` frees memory.

## Model manager

```c
tr_model_manager* m = tr_mm_create("/…/models");
tr_mm_load_cached_manifest(m) || tr_mm_load_manifest_json(m, bundled_json);
tr_mm_load_manifest_json(m, fresh_json_from_network); tr_mm_save_manifest(m);   // optional refresh
char* status = tr_mm_status_json(m, /*deep_verify=*/0);
```

### Status JSON

```json
{
  "manifest_version": "1.1.0",
  "models_root": "/…/models",
  "pending_bytes": 268000000,
  "models": [
    {
      "id": "whisper-small-q5_1", "kind": "stt", "pair": "", "version": "1",
      "state": "missing",            // ready | missing | corrupt | update_available | unverified
      "needs_download": true,
      "detail": "missing: whisper-small-q5_1.bin",
      "installed_version": "",
      "install_path": "/…/models/stt/whisper-small-q5_1.bin",
      "total_bytes": 190085487,
      "downloads": [ { "url": "https://…/stt/whisper-small-q5_1.bin", "filename": "whisper-small-q5_1.bin",
                       "size_bytes": 190085487, "sha256": "…", "archive": false } ],
      "required_files": ["whisper-small-q5_1.bin"]
    },
    { "id": "nmt-ko-en", "kind": "nmt", "pair": "ko-en", … }
  ]
}
```

| state | meaning | quick check | deep check |
|---|---|---|---|
| `ready` | files present, installed record matches manifest | size | SHA-256 |
| `missing` | one or more required files absent | | |
| `corrupt` | size mismatch (quick) or hash mismatch (deep) | ✔ | ✔ |
| `update_available` | manifest version / hashes changed since install | | |
| `unverified` | files present but no `installed.json` record (side-loaded) | | deep → ready/corrupt |

### Download → verify → install flow (platform layer)

```
for each model with needs_download:
    staging = tr_mm_staging_dir(m, id)                     # <root>/staging/<id>/
    for each download item:
        HTTP GET (Range-resumable) → staging/<filename>
        if archive:
            tr_mm_verify_file(staging/<filename>, sha256, size)   # must be 1
            unzip into staging/ ; delete the zip
    tr_mm_install(m, id, staging, verify_hashes=1)         # re-hashes non-archive files,
                                                           # moves into place atomically,
                                                           # writes installed.json
```

`installed.json` stores `{id: {version, signature, installed_at}}` where `signature` is a
SHA-256 over the manifest entry's version + file hashes, so any manifest change is detected
without re-hashing gigabytes at startup.

## Session callbacks (Kotlin / Swift, 0.3.3+)

`TranslatorSession` wraps mic → engine → TTS on both platforms. It reports:

| callback | when |
|---|---|
| `onResult` | an utterance was transcribed and translated |
| `onNoSpeech` | an utterance held no speech, so `onResult` will not fire — clear any "translating" indicator here |
| `onError` | the engine or the microphone failed |
| `onSpeechState` | TTS playback started / finished (only when `speakResults` is on) |
| `onAudioLevel` | microphone loudness 0..1, ~25×/s, on the audio thread |

`speakResults` and `speechRate` are settable while running, so an app can mute mid-sentence.
Set `speakResults = false` and call `speak(text, lang)` yourself when the UI needs to replay
individual lines. `start()` may be called repeatedly (push-to-talk); it keeps a single worker.

## Build capabilities

`tr_build_capabilities()` → `{"rnnoise":true,"whisper":true,"ctranslate2":true,"sentencepiece":true,"version":"0.1.0"}`.
A `false` means that engine was not present under `third_party/` at build time and the
corresponding stage runs as a stub (desktop test builds).
