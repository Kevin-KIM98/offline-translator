// JNI bridge for com.offlinetranslator.NativeBridge (Kotlin `external` functions).
// Thin: converts Java types ↔ C API types; all logic lives in translator_c_api.
#include <jni.h>

#include <android/log.h>
#include <cstring>
#include <string>
#include <vector>

#include "translator_c_api.h"

#define LOG_TAG "OfflineTranslator"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---- UTF-8 helpers (avoid Modified-UTF-8 pitfalls of GetStringUTFChars for emoji etc.) ----

std::string jstringToUtf8(JNIEnv* env, jstring js) {
    if (!js) return {};
    jclass strClass = env->FindClass("java/lang/String");
    jmethodID getBytes = env->GetMethodID(strClass, "getBytes", "(Ljava/lang/String;)[B");
    jstring charset = env->NewStringUTF("UTF-8");
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(js, getBytes, charset));
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(strClass);
    if (!bytes) return {};
    const jsize len = env->GetArrayLength(bytes);
    std::string out(static_cast<std::size_t>(len), '\0');
    env->GetByteArrayRegion(bytes, 0, len, reinterpret_cast<jbyte*>(&out[0]));
    env->DeleteLocalRef(bytes);
    return out;
}

jstring utf8ToJstring(JNIEnv* env, const char* s) {
    if (!s) s = "";
    const jsize len = static_cast<jsize>(std::strlen(s));
    jbyteArray bytes = env->NewByteArray(len);
    env->SetByteArrayRegion(bytes, 0, len, reinterpret_cast<const jbyte*>(s));
    jclass strClass = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(strClass, ctor, bytes, charset));
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(strClass);
    env->DeleteLocalRef(bytes);
    return result;
}

// Takes ownership of a tr_* allocated string.
jstring takeString(JNIEnv* env, char* s) {
    jstring j = utf8ToJstring(env, s ? s : "");
    tr_string_free(s);
    return j;
}

struct OptString {
    std::string value;
    bool present;
    OptString(JNIEnv* env, jstring js) : value(jstringToUtf8(env, js)), present(js != nullptr) {}
    const char* c_str() const { return present ? value.c_str() : nullptr; }
};

// ---- Progress callback bridging to NativeBridge.ProgressListener ----

struct ProgressCtx {
    JNIEnv* env;
    jobject listener;
    jmethodID method;
};

int progressTrampoline(uint64_t done, uint64_t total, void* user) {
    auto* ctx = static_cast<ProgressCtx*>(user);
    if (!ctx || !ctx->listener) return 1;
    const jboolean cont = ctx->env->CallBooleanMethod(ctx->listener, ctx->method, static_cast<jlong>(done), static_cast<jlong>(total));
    if (ctx->env->ExceptionCheck()) {
        ctx->env->ExceptionClear();
        return 0;
    }
    return cont ? 1 : 0;
}

ProgressCtx makeProgress(JNIEnv* env, jobject listener) {
    ProgressCtx ctx{env, listener, nullptr};
    if (listener) {
        jclass cls = env->GetObjectClass(listener);
        ctx.method = env->GetMethodID(cls, "onProgress", "(JJ)Z");
        env->DeleteLocalRef(cls);
        if (!ctx.method) ctx.listener = nullptr;
    }
    return ctx;
}

inline tr_pipeline* P(jlong h) { return reinterpret_cast<tr_pipeline*>(h); }
inline tr_model_manager* M(jlong h) { return reinterpret_cast<tr_model_manager*>(h); }

} // namespace

extern "C" {

#define JNI_FN(ret, name) JNIEXPORT ret JNICALL Java_com_offlinetranslator_NativeBridge_##name

/* ---------------------------------------------------------------------- */
/* Library                                                                  */
/* ---------------------------------------------------------------------- */

JNI_FN(jstring, version)(JNIEnv* env, jobject) { return utf8ToJstring(env, tr_version()); }
JNI_FN(jstring, buildCapabilities)(JNIEnv* env, jobject) { return takeString(env, tr_build_capabilities()); }
JNI_FN(jstring, lastGlobalError)(JNIEnv* env, jobject) { return utf8ToJstring(env, tr_last_global_error()); }

/* ---------------------------------------------------------------------- */
/* Pipeline                                                                 */
/* ---------------------------------------------------------------------- */

JNI_FN(jlong, pipelineCreate)(JNIEnv* env, jobject, jstring whisperPath, jstring nmtRoot, jint nThreads, jboolean useGpu,
                             jboolean denoise, jint beam, jint maxLen, jstring pivots, jstring prompt, jboolean preloadAll,
                             jstring llmPath, jint backend, jint llmCtx) {
    OptString wp(env, whisperPath), nr(env, nmtRoot), pv(env, pivots), pr(env, prompt), lp(env, llmPath);
    tr_pipeline_config cfg;
    tr_pipeline_config_init(&cfg);
    cfg.whisper_model_path = wp.c_str();
    cfg.nmt_root_dir = nr.c_str();
    cfg.n_threads = nThreads;
    cfg.use_gpu = useGpu ? 1 : 0;
    cfg.enable_denoise = denoise ? 1 : 0;
    cfg.beam_size = beam;
    cfg.max_decoding_length = maxLen;
    cfg.pivot_langs = pv.c_str();
    cfg.initial_prompt = pr.c_str();
    cfg.preload_all_pairs = preloadAll ? 1 : 0;
    cfg.llm_model_path = lp.c_str();
    cfg.translation_backend = backend;
    if (llmCtx > 0) cfg.llm_context_size = llmCtx;
    tr_pipeline* p = tr_pipeline_create(&cfg);
    if (!p) LOGE("pipelineCreate failed: %s", tr_last_global_error());
    return reinterpret_cast<jlong>(p);
}

JNI_FN(void, pipelineDestroy)(JNIEnv*, jobject, jlong h) { tr_pipeline_destroy(P(h)); }
JNI_FN(jstring, pipelineLastError)(JNIEnv* env, jobject, jlong h) { return utf8ToJstring(env, tr_pipeline_last_error(P(h))); }
JNI_FN(jstring, pipelineCapabilities)(JNIEnv* env, jobject, jlong h) { return takeString(env, tr_pipeline_capabilities(P(h))); }

JNI_FN(jboolean, pipelineSetSegmenter)(JNIEnv*, jobject, jlong h, jfloat startTh, jfloat endTh, jint startFrames,
                                      jint endSilenceMs, jint minMs, jint maxMs, jint preRollMs) {
    tr_segmenter_config c;
    tr_segmenter_config_init(&c);
    c.start_threshold = startTh;
    c.end_threshold = endTh;
    c.start_frames = startFrames;
    c.end_silence_ms = endSilenceMs;
    c.min_utterance_ms = minMs;
    c.max_utterance_ms = maxMs;
    c.pre_roll_ms = preRollMs;
    return tr_pipeline_set_segmenter_config(P(h), &c) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, pipelineFeedAudio)(JNIEnv* env, jobject, jlong h, jshortArray pcm, jint n) {
    if (!pcm || n <= 0) return JNI_FALSE;
    jshort* data = env->GetShortArrayElements(pcm, nullptr);
    const jsize len = env->GetArrayLength(pcm);
    const size_t count = static_cast<size_t>(n < len ? n : len);
    const int r = tr_pipeline_feed_audio_i16(P(h), reinterpret_cast<const int16_t*>(data), count);
    env->ReleaseShortArrayElements(pcm, data, JNI_ABORT);
    return r ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, pipelineFeedAudioFloat)(JNIEnv* env, jobject, jlong h, jfloatArray pcm, jint n) {
    if (!pcm || n <= 0) return JNI_FALSE;
    jfloat* data = env->GetFloatArrayElements(pcm, nullptr);
    const jsize len = env->GetArrayLength(pcm);
    const size_t count = static_cast<size_t>(n < len ? n : len);
    const int r = tr_pipeline_feed_audio(P(h), data, count);
    env->ReleaseFloatArrayElements(pcm, data, JNI_ABORT);
    return r ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jint, pipelinePendingCount)(JNIEnv*, jobject, jlong h) { return tr_pipeline_pending_count(P(h)); }
JNI_FN(jboolean, pipelineFlushAudio)(JNIEnv*, jobject, jlong h) { return tr_pipeline_flush_audio(P(h)) ? JNI_TRUE : JNI_FALSE; }
JNI_FN(void, pipelineResetAudio)(JNIEnv*, jobject, jlong h) { tr_pipeline_reset_audio(P(h)); }

JNI_FN(jstring, pipelineProcessPending)(JNIEnv* env, jobject, jlong h, jstring src, jstring tgt) {
    OptString s(env, src), t(env, tgt);
    return takeString(env, tr_pipeline_process_pending(P(h), s.c_str(), t.c_str()));
}

JNI_FN(jstring, pipelineTranscribe)(JNIEnv* env, jobject, jlong h, jfloatArray pcm, jstring src) {
    OptString s(env, src);
    jfloat* data = pcm ? env->GetFloatArrayElements(pcm, nullptr) : nullptr;
    const jsize len = pcm ? env->GetArrayLength(pcm) : 0;
    char* r = tr_pipeline_transcribe(P(h), data, static_cast<size_t>(len), s.c_str());
    if (pcm) env->ReleaseFloatArrayElements(pcm, data, JNI_ABORT);
    return takeString(env, r);
}

JNI_FN(jstring, pipelineTranslateText)(JNIEnv* env, jobject, jlong h, jstring text, jstring src, jstring tgt) {
    OptString x(env, text), s(env, src), t(env, tgt);
    return takeString(env, tr_pipeline_translate_text(P(h), x.c_str(), s.c_str(), t.c_str()));
}

JNI_FN(jstring, pipelineProcessSpeech)(JNIEnv* env, jobject, jlong h, jfloatArray pcm, jstring src, jstring tgt) {
    OptString s(env, src), t(env, tgt);
    jfloat* data = pcm ? env->GetFloatArrayElements(pcm, nullptr) : nullptr;
    const jsize len = pcm ? env->GetArrayLength(pcm) : 0;
    char* r = tr_pipeline_process_speech(P(h), data, static_cast<size_t>(len), s.c_str(), t.c_str());
    if (pcm) env->ReleaseFloatArrayElements(pcm, data, JNI_ABORT);
    return takeString(env, r);
}

JNI_FN(jboolean, pipelinePreloadPair)(JNIEnv* env, jobject, jlong h, jstring src, jstring tgt) {
    OptString s(env, src), t(env, tgt);
    return tr_pipeline_preload_pair(P(h), s.c_str(), t.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(void, pipelineUnloadPair)(JNIEnv* env, jobject, jlong h, jstring src, jstring tgt) {
    OptString s(env, src), t(env, tgt);
    tr_pipeline_unload_pair(P(h), s.c_str(), t.c_str());
}

JNI_FN(jboolean, pipelineCanTranslate)(JNIEnv* env, jobject, jlong h, jstring src, jstring tgt) {
    OptString s(env, src), t(env, tgt);
    return tr_pipeline_can_translate(P(h), s.c_str(), t.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jstring, pipelineAvailablePairs)(JNIEnv* env, jobject, jlong h) { return takeString(env, tr_pipeline_available_pairs(P(h))); }

/* ---------------------------------------------------------------------- */
/* Model manager                                                            */
/* ---------------------------------------------------------------------- */

JNI_FN(jlong, mmCreate)(JNIEnv* env, jobject, jstring root) {
    OptString r(env, root);
    return reinterpret_cast<jlong>(tr_mm_create(r.c_str()));
}

JNI_FN(void, mmDestroy)(JNIEnv*, jobject, jlong h) { tr_mm_destroy(M(h)); }
JNI_FN(jstring, mmLastError)(JNIEnv* env, jobject, jlong h) { return utf8ToJstring(env, tr_mm_last_error(M(h))); }

JNI_FN(jboolean, mmLoadManifestFile)(JNIEnv* env, jobject, jlong h, jstring path) {
    OptString p(env, path);
    return tr_mm_load_manifest_file(M(h), p.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, mmLoadManifestJson)(JNIEnv* env, jobject, jlong h, jstring json) {
    OptString j(env, json);
    return tr_mm_load_manifest_json(M(h), j.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, mmLoadCachedManifest)(JNIEnv*, jobject, jlong h) { return tr_mm_load_cached_manifest(M(h)) ? JNI_TRUE : JNI_FALSE; }
JNI_FN(jboolean, mmSaveManifest)(JNIEnv*, jobject, jlong h) { return tr_mm_save_manifest(M(h)) ? JNI_TRUE : JNI_FALSE; }
JNI_FN(jstring, mmManifestVersion)(JNIEnv* env, jobject, jlong h) { return utf8ToJstring(env, tr_mm_manifest_version(M(h))); }
JNI_FN(jstring, mmStatusJson)(JNIEnv* env, jobject, jlong h, jboolean deep) { return takeString(env, tr_mm_status_json(M(h), deep ? 1 : 0)); }

JNI_FN(jstring, mmStatusForLanguagesJson)(JNIEnv* env, jobject, jlong h, jstring langs, jboolean deep, jint llmMode) {
    OptString l(env, langs);
    return takeString(env, tr_mm_status_for_languages_json(M(h), l.c_str(), deep ? 1 : 0, llmMode));
}

JNI_FN(jstring, mmLlmModelPath)(JNIEnv* env, jobject, jlong h) { return takeString(env, tr_mm_llm_model_path(M(h))); }

JNI_FN(jstring, mmStagingDir)(JNIEnv* env, jobject, jlong h, jstring id) {
    OptString i(env, id);
    return takeString(env, tr_mm_staging_dir(M(h), i.c_str()));
}

JNI_FN(jboolean, mmClearStaging)(JNIEnv* env, jobject, jlong h, jstring id) {
    OptString i(env, id);
    return tr_mm_clear_staging(M(h), i.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jint, mmVerifyFile)(JNIEnv* env, jobject, jlong h, jstring path, jstring sha256, jlong size, jobject listener) {
    OptString p(env, path), s(env, sha256);
    ProgressCtx ctx = makeProgress(env, listener);
    return tr_mm_verify_file(M(h), p.c_str(), s.c_str(), static_cast<uint64_t>(size < 0 ? 0 : size),
                             ctx.listener ? progressTrampoline : nullptr, &ctx);
}

JNI_FN(jboolean, mmInstall)(JNIEnv* env, jobject, jlong h, jstring id, jstring staged, jboolean verify, jobject listener) {
    OptString i(env, id), s(env, staged);
    ProgressCtx ctx = makeProgress(env, listener);
    return tr_mm_install(M(h), i.c_str(), s.c_str(), verify ? 1 : 0, ctx.listener ? progressTrampoline : nullptr, &ctx) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, mmRemove)(JNIEnv* env, jobject, jlong h, jstring id) {
    OptString i(env, id);
    return tr_mm_remove(M(h), i.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jstring, mmSttModelPath)(JNIEnv* env, jobject, jlong h) { return takeString(env, tr_mm_stt_model_path(M(h))); }
JNI_FN(jstring, mmNmtRootDir)(JNIEnv* env, jobject, jlong h) { return takeString(env, tr_mm_nmt_root_dir(M(h))); }

JNI_FN(jstring, sha256File)(JNIEnv* env, jobject, jstring path, jobject listener) {
    OptString p(env, path);
    ProgressCtx ctx = makeProgress(env, listener);
    char* hex = tr_sha256_file(p.c_str(), ctx.listener ? progressTrampoline : nullptr, &ctx);
    if (!hex) return nullptr;
    return takeString(env, hex);
}

} // extern "C"
