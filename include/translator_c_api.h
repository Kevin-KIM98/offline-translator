/*
 * Stable C ABI for the offline translation engine.
 * Consumed by the Android JNI bridge and the iOS Objective-C++ wrapper (and any other FFI).
 *
 * Conventions
 *   - Strings are UTF-8. Strings returned as `char*` are heap-allocated and must be released
 *     with tr_string_free(). Strings returned as `const char*` are owned by the engine.
 *   - Functions returning int: 1 = true/success, 0 = false, negative = error.
 *   - Structured results are JSON documents (see docs/API.md).
 *   - All handles are thread-safe; inference calls are serialized internally.
 */
#ifndef TRANSLATOR_C_API_H
#define TRANSLATOR_C_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(TRANSLATOR_BUILD_SHARED)
#    define TR_API __declspec(dllexport)
#  else
#    define TR_API
#  endif
#else
#  define TR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Library info                                                                */
/* ------------------------------------------------------------------------- */

TR_API const char* tr_version(void);
/* JSON: {"rnnoise":bool,"whisper":bool,"ctranslate2":bool,"sentencepiece":bool} */
TR_API char* tr_build_capabilities(void);
TR_API void tr_string_free(char* s);

/* ------------------------------------------------------------------------- */
/* Pipeline                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct tr_pipeline tr_pipeline;

typedef struct tr_pipeline_config {
    const char* whisper_model_path;   /* required for speech; may be NULL for text-only use */
    const char* nmt_root_dir;         /* <models>/nmt */
    int n_threads;                    /* 0 → default (4) */
    int use_gpu;                      /* 1 → Metal / Vulkan when compiled in */
    int enable_denoise;               /* 1 → RNNoise front-end */
    int beam_size;                    /* 0 → default (4) */
    int max_decoding_length;          /* 0 → default (256) */
    const char* pivot_langs;          /* comma-separated, e.g. "en,ko"; NULL → "en,ko" */
    const char* initial_prompt;       /* optional whisper prompt */
    int preload_all_pairs;            /* 1 → load every NMT pair at init */
    /* Quality knobs (0.2+); tr_pipeline_config_init() sets the defaults. */
    int stt_beam_size;                /* whisper beam width, 1 = greedy (default 5) */
    int use_default_prompts;          /* punctuated per-language whisper prompt (default 1) */
    int use_context_prompt;           /* feed the previous transcript as context (default 1) */
    int clean_transcripts;            /* hallucination / repetition filter (default 1) */
    int post_process_translations;    /* spacing / capitalization fixes (default 1) */
    int no_repeat_ngram_size;         /* NMT n-gram blocking (default 3, 0 = off) */
    float repetition_penalty;         /* NMT (default 1.1) */
    float no_speech_threshold;        /* drop whisper segments above this (default 0.85) */
    int stt_adaptive_audio_ctx;       /* shrink whisper encoder window to utterance length (default 1) */
    /* LLM translation backend (0.3+) */
    const char* llm_model_path;       /* GGUF file; NULL → Marian only */
    int translation_backend;          /* 0 auto (Marian if a pair exists, else LLM), 1 Marian only, 2 LLM only */
    int llm_context_size;             /* default 1024 */
    int llm_max_output_tokens;        /* default 256 */
    float llm_temperature;            /* default 0 (greedy) */
    int llm_gpu_layers;               /* default 99 */
    const char* llm_system_prompt;    /* optional override of the translation instruction */
} tr_pipeline_config;

typedef struct tr_segmenter_config {
    float start_threshold;   /* default 0.60 */
    float end_threshold;     /* default 0.35 */
    int start_frames;        /* default 3 */
    int end_silence_ms;      /* default 700 */
    int min_utterance_ms;    /* default 400 */
    int max_utterance_ms;    /* default 15000 */
    int pre_roll_ms;         /* default 300 */
} tr_segmenter_config;

TR_API void tr_pipeline_config_init(tr_pipeline_config* cfg);
TR_API void tr_segmenter_config_init(tr_segmenter_config* cfg);

/* Returns NULL on failure; call tr_last_global_error() for the reason. */
TR_API tr_pipeline* tr_pipeline_create(const tr_pipeline_config* cfg);
TR_API void tr_pipeline_destroy(tr_pipeline* p);
TR_API const char* tr_pipeline_last_error(tr_pipeline* p);
TR_API const char* tr_last_global_error(void);
TR_API char* tr_pipeline_capabilities(tr_pipeline* p); /* JSON */

TR_API int tr_pipeline_set_segmenter_config(tr_pipeline* p, const tr_segmenter_config* cfg);

/* Streaming ---------------------------------------------------------------- */
/* Denoise one 480-sample frame (in/out may alias). Returns VAD probability. */
TR_API float tr_pipeline_denoise_frame(tr_pipeline* p, const float* in480, float* out480);
/* Feed arbitrary-length 16 kHz mono float PCM. Returns 1 when an utterance is ready. */
TR_API int tr_pipeline_feed_audio(tr_pipeline* p, const float* pcm, size_t n);
/* Same, for 16-bit PCM (converted internally). */
TR_API int tr_pipeline_feed_audio_i16(tr_pipeline* p, const int16_t* pcm, size_t n);
TR_API int tr_pipeline_pending_count(tr_pipeline* p);
TR_API int tr_pipeline_flush_audio(tr_pipeline* p);
TR_API void tr_pipeline_reset_audio(tr_pipeline* p);
/* Pops the oldest utterance and runs STT+NMT. Returns TranslationResult JSON. Blocking. */
TR_API char* tr_pipeline_process_pending(tr_pipeline* p, const char* source_lang, const char* target_lang);

/* One-shot ----------------------------------------------------------------- */
/* {"ok":bool,"text":"...","detected_lang":"ko","elapsed_ms":123.4,"error":"..."} */
TR_API char* tr_pipeline_transcribe(tr_pipeline* p, const float* pcm, size_t n, const char* source_lang);
/* TranslationResult JSON */
TR_API char* tr_pipeline_translate_text(tr_pipeline* p, const char* text, const char* source_lang, const char* target_lang);
TR_API char* tr_pipeline_process_speech(tr_pipeline* p, const float* pcm, size_t n, const char* source_lang, const char* target_lang);

/* NMT management ----------------------------------------------------------- */
TR_API int tr_pipeline_preload_pair(tr_pipeline* p, const char* src, const char* tgt);
TR_API void tr_pipeline_unload_pair(tr_pipeline* p, const char* src, const char* tgt);
TR_API int tr_pipeline_can_translate(tr_pipeline* p, const char* src, const char* tgt);
/* JSON array of "xx-yy" strings */
TR_API char* tr_pipeline_available_pairs(tr_pipeline* p);

/* ------------------------------------------------------------------------- */
/* Model manager                                                               */
/* ------------------------------------------------------------------------- */

typedef struct tr_model_manager tr_model_manager;
/* Return 0 to abort. */
typedef int (*tr_progress_fn)(uint64_t bytes_done, uint64_t bytes_total, void* user);

TR_API tr_model_manager* tr_mm_create(const char* models_root);
TR_API void tr_mm_destroy(tr_model_manager* m);
TR_API const char* tr_mm_last_error(tr_model_manager* m);

/* Manifest: load from a file path or a JSON string; the last loaded manifest is cached at
   <models_root>/manifest.json by tr_mm_save_manifest() for offline starts. */
TR_API int tr_mm_load_manifest_file(tr_model_manager* m, const char* path);
TR_API int tr_mm_load_manifest_json(tr_model_manager* m, const char* json);
TR_API int tr_mm_load_cached_manifest(tr_model_manager* m);
TR_API int tr_mm_save_manifest(tr_model_manager* m);
TR_API const char* tr_mm_manifest_version(tr_model_manager* m);

/* Status JSON: {"manifest_version","models_root","pending_bytes","models":[ModelStatus...]}
   deep_verify=1 re-hashes installed files (slow; run on a background thread). */
TR_API char* tr_mm_status_json(tr_model_manager* m, int deep_verify);
/* Same but limited to STT/tokenizer + NMT pairs whose both languages are in `langs_csv` ("ko,en").
   llm_mode: 0 = include the LLM only when some requested direction has no Marian route,
             1 = always include the LLM, 2 = never. */
TR_API char* tr_mm_status_for_languages_json(tr_model_manager* m, const char* langs_csv, int deep_verify, int llm_mode);
TR_API char* tr_mm_llm_model_path(tr_model_manager* m);   /* "" when the manifest has no llm entry */

/* Download staging directory for a model id (created). Caller must tr_string_free(). */
TR_API char* tr_mm_staging_dir(tr_model_manager* m, const char* id);
TR_API int tr_mm_clear_staging(tr_model_manager* m, const char* id /* NULL = all */);

/* 1 ok, 0 mismatch (see tr_mm_last_error), -1 io error, -2 aborted. expected_size 0 = skip. */
TR_API int tr_mm_verify_file(tr_model_manager* m, const char* path, const char* expected_sha256,
                             uint64_t expected_size, tr_progress_fn progress, void* user);

/* Moves the staged file/dir into place after verification and records installed.json. */
TR_API int tr_mm_install(tr_model_manager* m, const char* id, const char* staged_path, int verify_hashes,
                         tr_progress_fn progress, void* user);
TR_API int tr_mm_remove(tr_model_manager* m, const char* id);

TR_API char* tr_mm_stt_model_path(tr_model_manager* m);
TR_API char* tr_mm_nmt_root_dir(tr_model_manager* m);

/* Standalone helper: SHA-256 hex of a file (NULL on error). */
TR_API char* tr_sha256_file(const char* path, tr_progress_fn progress, void* user);

#ifdef __cplusplus
}
#endif

#endif /* TRANSLATOR_C_API_H */
