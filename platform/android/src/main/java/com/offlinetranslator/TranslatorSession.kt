package com.offlinetranslator

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Glue for a live conversation: mic → engine (denoise + VAD + STT + NMT) → OS TTS.
 *
 *   val session = TranslatorSession(context, repo.pipelineConfig())
 *   session.onResult = { r -> showTranscript(r) }
 *   session.start(sourceLang = "auto", targetLang = "en")
 *   ...
 *   session.stop(); session.close()
 */
class TranslatorSession(
    context: Context,
    config: PipelineConfig,
    speakResults: Boolean = true,
) : AutoCloseable {

    val translator = OfflineTranslator(config)
    /** OS speech synthesis, shared with [speak]; use it to check installed voices. */
    val tts = OfflineTTSManager(context)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val ready = Channel<Unit>(Channel.CONFLATED)
    private var worker: Job? = null

    @Volatile var sourceLang: String = "auto"
    @Volatile var targetLang: String = "en"

    /** Speak each translation through the OS TTS. Can be toggled while running (mute). */
    @Volatile var speakResults: Boolean = speakResults

    /** Playback speed for spoken translations (1.0 = normal). */
    @Volatile var speechRate: Float = 1.0f

    var onResult: ((TranslationResult) -> Unit)? = null

    /**
     * An utterance was processed but held no speech, so [onResult] will not fire for it.
     * UIs that show a "translating" indicator need this to clear it.
     */
    var onNoSpeech: (() -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onSpeechState: ((speaking: Boolean) -> Unit)? = null

    /**
     * Microphone loudness in 0..1, ~25×/s while capturing, for a level meter.
     * Called on the audio thread — hop to your UI dispatcher.
     */
    var onAudioLevel: ((Float) -> Unit)? = null

    private var levelFrames = 0

    private val capture = AudioCapture(context, onFrames = { pcm, n ->
        // Runs on the audio thread: cheap denoise + segmentation only.
        onAudioLevel?.let { cb ->
            if (++levelFrames >= 2) {
                levelFrames = 0
                var sum = 0.0
                for (i in 0 until n) {
                    val s = pcm[i].toDouble() / 32768.0
                    sum += s * s
                }
                val rms = if (n > 0) kotlin.math.sqrt(sum / n) else 0.0
                // Perceptual-ish curve: -50 dBFS .. 0 dBFS mapped to 0..1.
                val db = 20.0 * kotlin.math.log10(rms.coerceAtLeast(1e-6))
                cb(((db + 50.0) / 50.0).coerceIn(0.0, 1.0).toFloat())
            }
        }
        if (translator.feedAudio(pcm, n)) ready.trySend(Unit)
    }, onError = { onError?.invoke(it) })

    val isRunning: Boolean get() = capture.isRunning

    fun start(sourceLang: String = this.sourceLang, targetLang: String = this.targetLang): Boolean {
        this.sourceLang = sourceLang
        this.targetLang = targetLang
        if (capture.isRunning) return true
        translator.resetAudio()
        // Push-to-talk restarts capture many times; keep a single consumer of [ready].
        if (worker?.isActive == true) return capture.start()
        worker = scope.launch {
            for (unit in ready) {
                while (translator.pendingCount > 0) {
                    // Pause TTS so the mic does not pick up our own voice.
                    tts.stop()
                    val r = translator.processPending(this@TranslatorSession.sourceLang, this@TranslatorSession.targetLang)
                    if (!r.ok) onError?.invoke(r.error ?: "unknown error")
                    if (r.isEmpty) {
                        onNoSpeech?.invoke()
                        continue
                    }
                    onResult?.invoke(r)
                    if (r.ok && speakResults && r.translatedText.isNotBlank()) {
                        onSpeechState?.invoke(true)
                        tts.speakAndWait(r.translatedText, r.targetLang, speechRate)
                        onSpeechState?.invoke(false)
                    }
                }
            }
        }
        return capture.start()
    }

    /** Push-to-talk release: close the current utterance immediately. */
    fun endUtterance() {
        // Nothing is queued when the audio was too short or too quiet to be speech. That is not
        // an error, but a UI showing progress needs to hear about it or it waits forever.
        if (translator.flushAudio()) ready.trySend(Unit) else onNoSpeech?.invoke()
    }

    fun stop() {
        capture.stop()
        onAudioLevel?.invoke(0f)
        endUtterance()
    }

    /** Text-only path (keyboard input). */
    suspend fun translate(text: String, src: String, tgt: String): TranslationResult = withContext(Dispatchers.Default) {
        translator.translateText(text, src, tgt)
    }

    suspend fun speak(text: String, lang: String, rate: Float = speechRate) = tts.speakAndWait(text, lang, rate)

    /** Cuts off whatever the TTS is currently saying. */
    fun stopSpeaking() = tts.stop()

    override fun close() {
        stop()
        worker?.cancel()
        scope.cancel()
        tts.shutdown()
        translator.close()
    }

    companion object {
        /**
         * One-call setup: fetch the manifest, download whatever the given languages need
         * (with progress), and return a ready session. Safe to call on every launch — models
         * already installed are skipped.
         *
         *   val session = TranslatorSession.prepare(context, listOf("ko", "en")) { done, total -> ... }
         *   session.onResult = { r -> ... }
         *   session.start(sourceLang = "ko", targetLang = "en")
         */
        suspend fun prepare(
            context: Context,
            languages: List<String>,
            manifestUrl: String? = ModelRepository.DEFAULT_MANIFEST_URL,
            speakResults: Boolean = true,
            llmMode: LlmMode = LlmMode.IF_NEEDED,
            backend: TranslationBackend = TranslationBackend.AUTO,
            onProgress: ((bytesDone: Long, bytesTotal: Long) -> Unit)? = null,
        ): TranslatorSession {
            val repo = ModelRepository(context, manifestUrl = manifestUrl)
            try {
                repo.refreshManifest()
                if (!repo.hasManifest()) throw TranslatorException("no model manifest available (offline on first launch?)")
                val effectiveLlm = if (backend == TranslationBackend.LLM) LlmMode.ALWAYS else llmMode
                val needed = repo.statusForLanguages(languages, llmMode = effectiveLlm).filter { it.needsDownload }
                val total = needed.sumOf { it.totalBytes }
                if (needed.isNotEmpty()) {
                    if (repo.freeBytes() < total + 50L * 1024 * 1024) {
                        throw TranslatorException("not enough storage: need ${total / 1_000_000} MB")
                    }
                    var completedBytes = 0L
                    var failure: String? = null
                    repo.installAll(needed).collect { ev ->
                        when (ev) {
                            is InstallEvent.Downloading -> onProgress?.invoke(completedBytes + ev.bytesDone, total)
                            is InstallEvent.Installed -> {
                                completedBytes += needed.first { it.id == ev.id }.totalBytes
                                onProgress?.invoke(completedBytes, total)
                            }
                            is InstallEvent.Failed -> failure = "${ev.id}: ${ev.reason}"
                            else -> {}
                        }
                    }
                    failure?.let { throw TranslatorException("model download failed: $it") }
                }
                return TranslatorSession(context, repo.pipelineConfig(backend = backend), speakResults)
            } finally {
                repo.close()
            }
        }
    }
}
