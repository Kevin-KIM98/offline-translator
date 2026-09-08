#include "translator_c_api.h"

#include "translator/FileUtil.hpp"
#include "translator/ModelManager.hpp"
#include "translator/Sha256.hpp"
#include "translator/TranslationPipeline.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace translator;

#ifndef TRANSLATOR_VERSION_STRING
#define TRANSLATOR_VERSION_STRING "0.3.0"
#endif

struct tr_pipeline {
    TranslationPipeline impl;
};

struct tr_model_manager {
    ModelManager impl;
    std::string lastError;
    std::mutex mutex;
    explicit tr_model_manager(const char* root) : impl(root ? root : "") {}
};

namespace {

std::mutex gErrorMutex;
std::string gLastError;

void setGlobalError(const std::string& e) {
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gLastError = e;
}

char* dupString(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

std::string safe(const char* s, const char* def = "") { return s ? std::string(s) : std::string(def); }

std::vector<std::string> splitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto a = item.find_first_not_of(" \t");
        const auto b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

ModelManager::ProgressFn wrapProgress(tr_progress_fn fn, void* user) {
    if (!fn) return nullptr;
    return [fn, user](std::uint64_t done, std::uint64_t total) { return fn(done, total, user) != 0; };
}

char* sttResultJson(const SttResult& r) {
    Json j = Json::object();
    j.set("ok", r.ok);
    if (!r.error.empty()) j.set("error", r.error);
    j.set("text", r.text);
    j.set("detected_lang", r.detectedLang);
    j.set("elapsed_ms", r.elapsedMs);
    return dupString(j.dump());
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Library info                                                                */
/* ------------------------------------------------------------------------- */

extern "C" {

TR_API const char* tr_version(void) { return TRANSLATOR_VERSION_STRING; }

TR_API char* tr_build_capabilities(void) {
    Json j = Json::object();
#if TRANSLATOR_HAS_RNNOISE
    j.set("rnnoise", true);
#else
    j.set("rnnoise", false);
#endif
#if TRANSLATOR_HAS_WHISPER
    j.set("whisper", true);
#else
    j.set("whisper", false);
#endif
#if TRANSLATOR_HAS_CTRANSLATE2
    j.set("ctranslate2", true);
#else
    j.set("ctranslate2", false);
#endif
#if TRANSLATOR_HAS_SENTENCEPIECE
    j.set("sentencepiece", true);
#else
    j.set("sentencepiece", false);
#endif
#if TRANSLATOR_HAS_LLAMA
    j.set("llama", true);
#else
    j.set("llama", false);
#endif
    j.set("version", TRANSLATOR_VERSION_STRING);
    return dupString(j.dump());
}

TR_API void tr_string_free(char* s) { std::free(s); }

TR_API const char* tr_last_global_error(void) {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lock(gErrorMutex);
    copy = gLastError;
    return copy.c_str();
}

/* ------------------------------------------------------------------------- */
/* Pipeline                                                                    */
/* ------------------------------------------------------------------------- */

TR_API void tr_pipeline_config_init(tr_pipeline_config* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->n_threads = 4;
    cfg->use_gpu = 1;
    cfg->enable_denoise = 1;
    cfg->beam_size = 4;
    cfg->max_decoding_length = 256;
    cfg->stt_beam_size = 5;
    cfg->use_default_prompts = 1;
    cfg->use_context_prompt = 1;
    cfg->clean_transcripts = 1;
    cfg->post_process_translations = 1;
    cfg->no_repeat_ngram_size = 3;
    cfg->repetition_penalty = 1.1f;
    cfg->no_speech_threshold = 0.85f;
    cfg->stt_adaptive_audio_ctx = 1;
    cfg->llm_model_path = nullptr;
    cfg->translation_backend = 0;
    cfg->llm_context_size = 1024;
    cfg->llm_max_output_tokens = 256;
    cfg->llm_temperature = 0.0f;
    cfg->llm_gpu_layers = 99;
    cfg->llm_system_prompt = nullptr;
}

TR_API void tr_segmenter_config_init(tr_segmenter_config* cfg) {
    if (!cfg) return;
    const SegmenterConfig d;
    cfg->start_threshold = d.startThreshold;
    cfg->end_threshold = d.endThreshold;
    cfg->start_frames = d.startFrames;
    cfg->end_silence_ms = d.endSilenceMs;
    cfg->min_utterance_ms = d.minUtteranceMs;
    cfg->max_utterance_ms = d.maxUtteranceMs;
    cfg->pre_roll_ms = d.preRollMs;
}

TR_API tr_pipeline* tr_pipeline_create(const tr_pipeline_config* cfg) {
    if (!cfg) {
        setGlobalError("tr_pipeline_create: null config");
        return nullptr;
    }
    PipelineConfig c;
    c.whisperModelPath = safe(cfg->whisper_model_path);
    c.nmtRootDir = safe(cfg->nmt_root_dir);
    c.nThreads = cfg->n_threads > 0 ? cfg->n_threads : 4;
    c.useGpu = cfg->use_gpu != 0;
    c.enableDenoise = cfg->enable_denoise != 0;
    c.beamSize = cfg->beam_size > 0 ? cfg->beam_size : 4;
    c.maxDecodingLength = cfg->max_decoding_length > 0 ? cfg->max_decoding_length : 256;
    c.sttBeamSize = cfg->stt_beam_size > 0 ? cfg->stt_beam_size : 1;
    c.useDefaultPrompts = cfg->use_default_prompts != 0;
    c.useContextPrompt = cfg->use_context_prompt != 0;
    c.cleanTranscripts = cfg->clean_transcripts != 0;
    c.postProcessTranslations = cfg->post_process_translations != 0;
    c.noRepeatNgramSize = cfg->no_repeat_ngram_size;
    c.repetitionPenalty = cfg->repetition_penalty > 0.0f ? cfg->repetition_penalty : 1.0f;
    c.noSpeechThreshold = cfg->no_speech_threshold;
    c.sttAdaptiveAudioContext = cfg->stt_adaptive_audio_ctx != 0;
    c.llmModelPath = safe(cfg->llm_model_path);
    c.backend = cfg->translation_backend == 2 ? TranslationBackend::Llm : cfg->translation_backend == 1 ? TranslationBackend::Marian : TranslationBackend::Auto;
    c.llmContextSize = cfg->llm_context_size > 0 ? cfg->llm_context_size : 1024;
    c.llmMaxOutputTokens = cfg->llm_max_output_tokens > 0 ? cfg->llm_max_output_tokens : 256;
    c.llmTemperature = cfg->llm_temperature;
    c.llmGpuLayers = cfg->llm_gpu_layers;
    c.llmSystemPrompt = safe(cfg->llm_system_prompt);
    if (cfg->pivot_langs && *cfg->pivot_langs) c.pivotLangs = splitCsv(cfg->pivot_langs);
    c.initialPrompt = safe(cfg->initial_prompt);
    c.preloadAllPairs = cfg->preload_all_pairs != 0;

    auto* p = new tr_pipeline();
    std::string err;
    if (!p->impl.initialize(c, &err)) {
        setGlobalError(err);
        delete p;
        return nullptr;
    }
    return p;
}

TR_API void tr_pipeline_destroy(tr_pipeline* p) { delete p; }

TR_API const char* tr_pipeline_last_error(tr_pipeline* p) {
    static thread_local std::string copy;
    copy = p ? p->impl.lastError() : "null pipeline";
    return copy.c_str();
}

TR_API char* tr_pipeline_capabilities(tr_pipeline* p) {
    if (!p) return tr_build_capabilities();
    return dupString(p->impl.capabilities().dump());
}

TR_API int tr_pipeline_set_segmenter_config(tr_pipeline* p, const tr_segmenter_config* cfg) {
    if (!p || !cfg) return 0;
    SegmenterConfig s;
    s.startThreshold = cfg->start_threshold;
    s.endThreshold = cfg->end_threshold;
    s.startFrames = cfg->start_frames;
    s.endSilenceMs = cfg->end_silence_ms;
    s.minUtteranceMs = cfg->min_utterance_ms;
    s.maxUtteranceMs = cfg->max_utterance_ms;
    s.preRollMs = cfg->pre_roll_ms;
    p->impl.setSegmenterConfig(s);
    return 1;
}

TR_API float tr_pipeline_denoise_frame(tr_pipeline* p, const float* in480, float* out480) {
    if (!p || !in480 || !out480) return 0.0f;
    return p->impl.denoiseFrame(in480, out480);
}

TR_API int tr_pipeline_feed_audio(tr_pipeline* p, const float* pcm, size_t n) {
    if (!p || !pcm || n == 0) return 0;
    return p->impl.feedAudio(pcm, n) ? 1 : 0;
}

TR_API int tr_pipeline_feed_audio_i16(tr_pipeline* p, const int16_t* pcm, size_t n) {
    if (!p || !pcm || n == 0) return 0;
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(pcm[i]) / 32768.0f;
    return p->impl.feedAudio(f.data(), f.size()) ? 1 : 0;
}

TR_API int tr_pipeline_pending_count(tr_pipeline* p) {
    return p ? static_cast<int>(p->impl.pendingUtteranceCount()) : 0;
}

TR_API int tr_pipeline_flush_audio(tr_pipeline* p) { return (p && p->impl.flushAudio()) ? 1 : 0; }

TR_API void tr_pipeline_reset_audio(tr_pipeline* p) {
    if (p) p->impl.resetAudio();
}

TR_API char* tr_pipeline_process_pending(tr_pipeline* p, const char* source_lang, const char* target_lang) {
    if (!p) return dupString("{\"ok\":false,\"error\":\"null pipeline\"}");
    return dupString(p->impl.processPendingUtterance(safe(source_lang, "auto"), safe(target_lang, "en")).toJson().dump());
}

TR_API char* tr_pipeline_transcribe(tr_pipeline* p, const float* pcm, size_t n, const char* source_lang) {
    if (!p) return dupString("{\"ok\":false,\"error\":\"null pipeline\"}");
    return sttResultJson(p->impl.transcribe(pcm, n, safe(source_lang, "auto")));
}

TR_API char* tr_pipeline_translate_text(tr_pipeline* p, const char* text, const char* source_lang, const char* target_lang) {
    if (!p) return dupString("{\"ok\":false,\"error\":\"null pipeline\"}");
    return dupString(p->impl.translateText(safe(text), safe(source_lang), safe(target_lang, "en")).toJson().dump());
}

TR_API char* tr_pipeline_process_speech(tr_pipeline* p, const float* pcm, size_t n, const char* source_lang, const char* target_lang) {
    if (!p) return dupString("{\"ok\":false,\"error\":\"null pipeline\"}");
    return dupString(p->impl.processSpeechToTranslation(pcm, n, safe(source_lang, "auto"), safe(target_lang, "en")).toJson().dump());
}

TR_API int tr_pipeline_preload_pair(tr_pipeline* p, const char* src, const char* tgt) {
    if (!p || !src || !tgt) return 0;
    return p->impl.preloadPair(src, tgt) ? 1 : 0;
}

TR_API void tr_pipeline_unload_pair(tr_pipeline* p, const char* src, const char* tgt) {
    if (p && src && tgt) p->impl.unloadPair(src, tgt);
}

TR_API int tr_pipeline_can_translate(tr_pipeline* p, const char* src, const char* tgt) {
    if (!p || !src || !tgt) return 0;
    return p->impl.canTranslate(src, tgt) ? 1 : 0;
}

TR_API char* tr_pipeline_available_pairs(tr_pipeline* p) {
    Json arr = Json::array();
    if (p)
        for (const auto& s : p->impl.availablePairs()) arr.push_back(s);
    return dupString(arr.dump());
}

/* ------------------------------------------------------------------------- */
/* Model manager                                                               */
/* ------------------------------------------------------------------------- */

TR_API tr_model_manager* tr_mm_create(const char* models_root) {
    if (!models_root || !*models_root) {
        setGlobalError("tr_mm_create: models_root required");
        return nullptr;
    }
    fs::makeDirs(models_root);
    return new tr_model_manager(models_root);
}

TR_API void tr_mm_destroy(tr_model_manager* m) { delete m; }

TR_API const char* tr_mm_last_error(tr_model_manager* m) {
    static thread_local std::string copy;
    if (!m) return "null model manager";
    std::lock_guard<std::mutex> lock(m->mutex);
    copy = m->lastError;
    return copy.c_str();
}

TR_API int tr_mm_load_manifest_file(tr_model_manager* m, const char* path) {
    if (!m || !path) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.loadManifestFile(path, &m->lastError) ? 1 : 0;
}

TR_API int tr_mm_load_manifest_json(tr_model_manager* m, const char* json) {
    if (!m || !json) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.loadManifestJson(json, &m->lastError) ? 1 : 0;
}

TR_API int tr_mm_load_cached_manifest(tr_model_manager* m) {
    if (!m) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.loadCachedManifest(&m->lastError) ? 1 : 0;
}

TR_API int tr_mm_save_manifest(tr_model_manager* m) {
    if (!m) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.saveManifest(&m->lastError) ? 1 : 0;
}

TR_API const char* tr_mm_manifest_version(tr_model_manager* m) {
    static thread_local std::string copy;
    if (!m) return "";
    std::lock_guard<std::mutex> lock(m->mutex);
    copy = m->impl.manifestVersion();
    return copy.c_str();
}

TR_API char* tr_mm_status_json(tr_model_manager* m, int deep_verify) {
    if (!m) return dupString("{\"models\":[]}");
    std::lock_guard<std::mutex> lock(m->mutex);
    return dupString(m->impl.statusJson(deep_verify != 0).dump());
}

TR_API char* tr_mm_status_for_languages_json(tr_model_manager* m, const char* langs_csv, int deep_verify, int llm_mode) {
    if (!m) return dupString("{\"models\":[]}");
    std::lock_guard<std::mutex> lock(m->mutex);
    Json j = Json::object();
    j.set("manifest_version", m->impl.manifestVersion());
    j.set("models_root", m->impl.modelsRoot());
    Json arr = Json::array();
    std::uint64_t pendingBytes = 0;
    const auto mode = llm_mode == 1 ? ModelManager::LlmMode::Always : llm_mode == 2 ? ModelManager::LlmMode::Never : ModelManager::LlmMode::IfNeeded;
    for (const auto& s : m->impl.statusForLanguages(splitCsv(safe(langs_csv)), deep_verify != 0, mode)) {
        if (s.needsDownload()) pendingBytes += s.entry.totalBytes();
        arr.push_back(s.toJson());
    }
    j.set("pending_bytes", pendingBytes);
    j.set("models", arr);
    return dupString(j.dump());
}

TR_API char* tr_mm_staging_dir(tr_model_manager* m, const char* id) {
    if (!m || !id) return nullptr;
    std::lock_guard<std::mutex> lock(m->mutex);
    return dupString(m->impl.stagingDir(id));
}

TR_API int tr_mm_clear_staging(tr_model_manager* m, const char* id) {
    if (!m) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.clearStaging(safe(id)) ? 1 : 0;
}

TR_API int tr_mm_verify_file(tr_model_manager* m, const char* path, const char* expected_sha256,
                             uint64_t expected_size, tr_progress_fn progress, void* user) {
    if (!m || !path) return -1;
    // Hashing a 1 GB file can take a while — don't hold the manager lock for it.
    const VerifyResult r = m->impl.verifyFile(path, safe(expected_sha256), expected_size, wrapProgress(progress, user));
    std::lock_guard<std::mutex> lock(m->mutex);
    switch (r) {
        case VerifyResult::Ok: m->lastError.clear(); return 1;
        case VerifyResult::Aborted: m->lastError = "aborted"; return -2;
        case VerifyResult::IoError: m->lastError = "io error reading " + std::string(path); return -1;
        default: m->lastError = verifyResultName(r); return 0;
    }
}

TR_API int tr_mm_install(tr_model_manager* m, const char* id, const char* staged_path, int verify_hashes,
                         tr_progress_fn progress, void* user) {
    if (!m || !id || !staged_path) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.install(id, staged_path, verify_hashes != 0, wrapProgress(progress, user), &m->lastError) ? 1 : 0;
}

TR_API int tr_mm_remove(tr_model_manager* m, const char* id) {
    if (!m || !id) return 0;
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->impl.remove(id, &m->lastError) ? 1 : 0;
}

TR_API char* tr_mm_stt_model_path(tr_model_manager* m) {
    if (!m) return nullptr;
    std::lock_guard<std::mutex> lock(m->mutex);
    return dupString(m->impl.sttModelPath());
}

TR_API char* tr_mm_llm_model_path(tr_model_manager* m) {
    if (!m) return nullptr;
    std::lock_guard<std::mutex> lock(m->mutex);
    return dupString(m->impl.llmModelPath());
}

TR_API char* tr_mm_nmt_root_dir(tr_model_manager* m) {
    if (!m) return nullptr;
    std::lock_guard<std::mutex> lock(m->mutex);
    return dupString(m->impl.nmtRootDir());
}

TR_API char* tr_sha256_file(const char* path, tr_progress_fn progress, void* user) {
    if (!path) return nullptr;
    const std::string hex = Sha256::hashFile(path, wrapProgress(progress, user));
    return hex.empty() ? nullptr : dupString(hex);
}

} // extern "C"
