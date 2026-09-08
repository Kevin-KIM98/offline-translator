package com.offlinetranslator

/**
 * Raw JNI surface over translator_c_api.h. Prefer [OfflineTranslator] / [ModelRepository],
 * which wrap these handles in typed, coroutine-friendly Kotlin.
 */
object NativeBridge {
    init {
        System.loadLibrary("offline_translator")
    }

    /** Return false to abort the operation. */
    fun interface ProgressListener {
        fun onProgress(bytesDone: Long, bytesTotal: Long): Boolean
    }

    // ---- Library ----
    external fun version(): String
    external fun buildCapabilities(): String
    external fun lastGlobalError(): String

    // ---- Pipeline ----
    external fun pipelineCreate(
        whisperPath: String?, nmtRoot: String, nThreads: Int, useGpu: Boolean, denoise: Boolean,
        beam: Int, maxLen: Int, pivots: String?, prompt: String?, preloadAll: Boolean,
        llmPath: String?, backend: Int, llmContextSize: Int,
    ): Long
    external fun pipelineDestroy(handle: Long)
    external fun pipelineLastError(handle: Long): String
    external fun pipelineCapabilities(handle: Long): String
    external fun pipelineSetSegmenter(
        handle: Long, startThreshold: Float, endThreshold: Float, startFrames: Int,
        endSilenceMs: Int, minUtteranceMs: Int, maxUtteranceMs: Int, preRollMs: Int,
    ): Boolean
    external fun pipelineFeedAudio(handle: Long, pcm: ShortArray, n: Int): Boolean
    external fun pipelineFeedAudioFloat(handle: Long, pcm: FloatArray, n: Int): Boolean
    external fun pipelinePendingCount(handle: Long): Int
    external fun pipelineFlushAudio(handle: Long): Boolean
    external fun pipelineResetAudio(handle: Long)
    external fun pipelineProcessPending(handle: Long, sourceLang: String, targetLang: String): String
    external fun pipelineTranscribe(handle: Long, pcm: FloatArray, sourceLang: String): String
    external fun pipelineTranslateText(handle: Long, text: String, sourceLang: String, targetLang: String): String
    external fun pipelineProcessSpeech(handle: Long, pcm: FloatArray, sourceLang: String, targetLang: String): String
    external fun pipelinePreloadPair(handle: Long, src: String, tgt: String): Boolean
    external fun pipelineUnloadPair(handle: Long, src: String, tgt: String)
    external fun pipelineCanTranslate(handle: Long, src: String, tgt: String): Boolean
    external fun pipelineAvailablePairs(handle: Long): String

    // ---- Model manager ----
    external fun mmCreate(modelsRoot: String): Long
    external fun mmDestroy(handle: Long)
    external fun mmLastError(handle: Long): String
    external fun mmLoadManifestFile(handle: Long, path: String): Boolean
    external fun mmLoadManifestJson(handle: Long, json: String): Boolean
    external fun mmLoadCachedManifest(handle: Long): Boolean
    external fun mmSaveManifest(handle: Long): Boolean
    external fun mmManifestVersion(handle: Long): String
    external fun mmStatusJson(handle: Long, deepVerify: Boolean): String
    /** llmMode: 0 include the LLM only when a requested direction has no Marian route, 1 always, 2 never */
    external fun mmStatusForLanguagesJson(handle: Long, langsCsv: String, deepVerify: Boolean, llmMode: Int): String
    external fun mmLlmModelPath(handle: Long): String
    external fun mmStagingDir(handle: Long, id: String): String
    external fun mmClearStaging(handle: Long, id: String?): Boolean
    /** 1 ok, 0 mismatch, -1 io error, -2 aborted */
    external fun mmVerifyFile(handle: Long, path: String, sha256: String, size: Long, listener: ProgressListener?): Int
    external fun mmInstall(handle: Long, id: String, stagedPath: String, verifyHashes: Boolean, listener: ProgressListener?): Boolean
    external fun mmRemove(handle: Long, id: String): Boolean
    external fun mmSttModelPath(handle: Long): String
    external fun mmNmtRootDir(handle: Long): String
    external fun sha256File(path: String, listener: ProgressListener?): String?
}
