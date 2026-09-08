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
    private val speakResults: Boolean = true,
) : AutoCloseable {

    val translator = OfflineTranslator(config)
    private val tts = OfflineTTSManager(context)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val ready = Channel<Unit>(Channel.CONFLATED)
    private var worker: Job? = null

    @Volatile var sourceLang: String = "auto"
    @Volatile var targetLang: String = "en"

    var onResult: ((TranslationResult) -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onSpeechState: ((speaking: Boolean) -> Unit)? = null

    private val capture = AudioCapture(context, onFrames = { pcm, n ->
        // Runs on the audio thread: cheap denoise + segmentation only.
        if (translator.feedAudio(pcm, n)) ready.trySend(Unit)
    }, onError = { onError?.invoke(it) })

    val isRunning: Boolean get() = capture.isRunning

    fun start(sourceLang: String = this.sourceLang, targetLang: String = this.targetLang): Boolean {
        this.sourceLang = sourceLang
        this.targetLang = targetLang
        if (capture.isRunning) return true
        translator.resetAudio()
        worker = scope.launch {
            for (unit in ready) {
                while (translator.pendingCount > 0) {
                    // Pause TTS so the mic does not pick up our own voice.
                    tts.stop()
                    val r = translator.processPending(this@TranslatorSession.sourceLang, this@TranslatorSession.targetLang)
                    if (!r.ok) onError?.invoke(r.error ?: "unknown error")
                    if (r.isEmpty) continue
                    onResult?.invoke(r)
                    if (r.ok && speakResults && r.translatedText.isNotBlank()) {
                        onSpeechState?.invoke(true)
                        tts.speakAndWait(r.translatedText, r.targetLang)
                        onSpeechState?.invoke(false)
                    }
                }
            }
        }
        return capture.start()
    }

    /** Push-to-talk release: close the current utterance immediately. */
    fun endUtterance() {
        if (translator.flushAudio()) ready.trySend(Unit)
    }

    fun stop() {
        capture.stop()
        endUtterance()
    }

    /** Text-only path (keyboard input). */
    suspend fun translate(text: String, src: String, tgt: String): TranslationResult = withContext(Dispatchers.Default) {
        translator.translateText(text, src, tgt)
    }

    suspend fun speak(text: String, lang: String) = tts.speakAndWait(text, lang)

    override fun close() {
        stop()
        worker?.cancel()
        scope.cancel()
        tts.shutdown()
        translator.close()
    }
}
