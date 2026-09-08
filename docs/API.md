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

## Build capabilities

`tr_build_capabilities()` → `{"rnnoise":true,"whisper":true,"ctranslate2":true,"sentencepiece":true,"version":"0.1.0"}`.
A `false` means that engine was not present under `third_party/` at build time and the
corresponding stage runs as a stub (desktop test builds).
