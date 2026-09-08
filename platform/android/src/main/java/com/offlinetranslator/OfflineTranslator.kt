package com.offlinetranslator

import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable

/** Which engine translates. */
enum class TranslationBackend(val native: Int) {
    /** Marian pair when one exists (direct or via English), otherwise the LLM. */
    AUTO(0),
    /** CTranslate2 OPUS-MT only. */
    MARIAN(1),
    /** LLM only: any→any in one hop, source language may be "auto". */
    LLM(2),
}

/** When the model repository should include the LLM for a language set. */
enum class LlmMode(val native: Int) { IF_NEEDED(0), ALWAYS(1), NEVER(2) }

/** Configuration mirrored from tr_pipeline_config. */
data class PipelineConfig(
    val whisperModelPath: String?,
    val nmtRootDir: String,
    val llmModelPath: String? = null,
    val backend: TranslationBackend = TranslationBackend.AUTO,
    val llmContextSize: Int = 1024,
    val nThreads: Int = Runtime.getRuntime().availableProcessors().coerceIn(2, 6),
    val useGpu: Boolean = true,
    val enableDenoise: Boolean = true,
    val beamSize: Int = 4,
    val maxDecodingLength: Int = 256,
    val pivotLangs: List<String> = listOf("en", "ko"),
    val initialPrompt: String? = null,
    val preloadAllPairs: Boolean = false,
)

data class SegmenterConfig(
    val startThreshold: Float = 0.60f,
    val endThreshold: Float = 0.35f,
    val startFrames: Int = 3,
    val endSilenceMs: Int = 700,
    val minUtteranceMs: Int = 400,
    val maxUtteranceMs: Int = 15_000,
    val preRollMs: Int = 300,
)

data class SttResult(
    val ok: Boolean,
    val text: String,
    val detectedLang: String,
    val elapsedMs: Double,
    val error: String?,
) {
    companion object {
        fun fromJson(json: String): SttResult {
            val o = JSONObject(json)
            return SttResult(
                ok = o.optBoolean("ok"),
                text = o.optString("text"),
                detectedLang = o.optString("detected_lang"),
                elapsedMs = o.optDouble("elapsed_ms", 0.0),
                error = o.optString("error").ifEmpty { null },
            )
        }
    }
}

data class TranslationResult(
    val ok: Boolean,
    val sourceText: String,
    val sourceLang: String,
    val targetLang: String,
    val translatedText: String,
    val route: List<String>,
    val sttMs: Double,
    val nmtMs: Double,
    val totalMs: Double,
    val error: String?,
) {
    /** True when STT produced no speech (silence) — nothing to translate, not an error. */
    val isEmpty: Boolean get() = ok && sourceText.isBlank()

    companion object {
        fun fromJson(json: String): TranslationResult {
            val o = JSONObject(json)
            val t = o.optJSONObject("timings") ?: JSONObject()
            return TranslationResult(
                ok = o.optBoolean("ok"),
                sourceText = o.optString("source_text"),
                sourceLang = o.optString("source_lang"),
                targetLang = o.optString("target_lang"),
                translatedText = o.optString("translated_text"),
                route = o.optJSONArray("route").toStringList(),
                sttMs = t.optDouble("stt_ms", 0.0),
                nmtMs = t.optDouble("nmt_ms", 0.0),
                totalMs = t.optDouble("total_ms", 0.0),
                error = o.optString("error").ifEmpty { null },
            )
        }
    }
}

internal fun JSONArray?.toStringList(): List<String> =
    if (this == null) emptyList() else List(length()) { getString(it) }

class TranslatorException(message: String) : RuntimeException(message)

/**
 * Typed wrapper around the native pipeline. All inference methods block — call them from
 * a background dispatcher (Dispatchers.Default). Audio feeding is cheap and may run on the
 * recording thread.
 */
class OfflineTranslator(config: PipelineConfig) : Closeable {
    private var handle: Long = NativeBridge.pipelineCreate(
        whisperPath = config.whisperModelPath,
        nmtRoot = config.nmtRootDir,
        nThreads = config.nThreads,
        useGpu = config.useGpu,
        denoise = config.enableDenoise,
        beam = config.beamSize,
        maxLen = config.maxDecodingLength,
        pivots = config.pivotLangs.joinToString(","),
        prompt = config.initialPrompt,
        preloadAll = config.preloadAllPairs,
        llmPath = config.llmModelPath,
        backend = config.backend.native,
        llmContextSize = config.llmContextSize,
    )

    init {
        if (handle == 0L) throw TranslatorException("pipeline init failed: ${NativeBridge.lastGlobalError()}")
    }

    private fun h(): Long = handle.takeIf { it != 0L } ?: throw IllegalStateException("translator closed")

    val lastError: String get() = NativeBridge.pipelineLastError(h())
    val capabilities: JSONObject get() = JSONObject(NativeBridge.pipelineCapabilities(h()))
    val availablePairs: List<String> get() = JSONArray(NativeBridge.pipelineAvailablePairs(h())).toStringList()

    fun setSegmenterConfig(c: SegmenterConfig): Boolean = NativeBridge.pipelineSetSegmenter(
        h(), c.startThreshold, c.endThreshold, c.startFrames, c.endSilenceMs, c.minUtteranceMs, c.maxUtteranceMs, c.preRollMs,
    )

    // ---- Streaming ----
    /** Feed 16 kHz mono PCM16. Returns true when an utterance is ready for [processPending]. */
    fun feedAudio(pcm: ShortArray, n: Int = pcm.size): Boolean = NativeBridge.pipelineFeedAudio(h(), pcm, n)
    fun feedAudio(pcm: FloatArray, n: Int = pcm.size): Boolean = NativeBridge.pipelineFeedAudioFloat(h(), pcm, n)
    val pendingCount: Int get() = NativeBridge.pipelinePendingCount(h())
    fun flushAudio(): Boolean = NativeBridge.pipelineFlushAudio(h())
    fun resetAudio() = NativeBridge.pipelineResetAudio(h())
    fun processPending(sourceLang: String = "auto", targetLang: String): TranslationResult =
        TranslationResult.fromJson(NativeBridge.pipelineProcessPending(h(), sourceLang, targetLang))

    // ---- One-shot ----
    fun transcribe(pcm: FloatArray, sourceLang: String = "auto"): SttResult =
        SttResult.fromJson(NativeBridge.pipelineTranscribe(h(), pcm, sourceLang))

    fun translateText(text: String, sourceLang: String, targetLang: String): TranslationResult =
        TranslationResult.fromJson(NativeBridge.pipelineTranslateText(h(), text, sourceLang, targetLang))

    fun processSpeech(pcm: FloatArray, sourceLang: String = "auto", targetLang: String): TranslationResult =
        TranslationResult.fromJson(NativeBridge.pipelineProcessSpeech(h(), pcm, sourceLang, targetLang))

    // ---- NMT management ----
    fun preloadPair(src: String, tgt: String): Boolean = NativeBridge.pipelinePreloadPair(h(), src, tgt)
    fun unloadPair(src: String, tgt: String) = NativeBridge.pipelineUnloadPair(h(), src, tgt)
    fun canTranslate(src: String, tgt: String): Boolean = NativeBridge.pipelineCanTranslate(h(), src, tgt)

    override fun close() {
        if (handle != 0L) {
            NativeBridge.pipelineDestroy(handle)
            handle = 0L
        }
    }

    companion object {
        val version: String get() = NativeBridge.version()
        val buildCapabilities: JSONObject get() = JSONObject(NativeBridge.buildCapabilities())
        val supportedLanguages: List<String> = listOf("ko", "en", "es", "vi", "th", "ja", "zh")
    }
}
