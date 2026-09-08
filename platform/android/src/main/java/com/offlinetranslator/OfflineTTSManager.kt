package com.offlinetranslator

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import java.util.Locale
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Wrapper over the OS TextToSpeech engine. Fully offline as long as the device has the
 * language's voice pack installed (Settings → System → Languages → Text-to-speech output).
 */
class OfflineTTSManager(context: Context) : TextToSpeech.OnInitListener {

    private val ready = CompletableDeferred<Boolean>()
    @Volatile private var isReady = false
    private val counter = AtomicInteger()
    private val tts: TextToSpeech = TextToSpeech(context.applicationContext, this)
    private val listeners = HashMap<String, () -> Unit>()

    init {
        tts.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
            override fun onStart(utteranceId: String?) {}
            override fun onDone(utteranceId: String?) { utteranceId?.let { listeners.remove(it)?.invoke() } }
            @Deprecated("Deprecated in Java")
            override fun onError(utteranceId: String?) { utteranceId?.let { listeners.remove(it)?.invoke() } }
            override fun onError(utteranceId: String?, errorCode: Int) { utteranceId?.let { listeners.remove(it)?.invoke() } }
        })
    }

    override fun onInit(status: Int) {
        isReady = status == TextToSpeech.SUCCESS
        ready.complete(isReady)
    }

    suspend fun awaitReady(): Boolean = ready.await()

    /** Whether the voice for [lang] is installed on the device (no download needed). */
    fun isLanguageAvailableOffline(lang: String): Boolean {
        val r = tts.isLanguageAvailable(localeFor(lang))
        if (r < TextToSpeech.LANG_AVAILABLE) return false
        val voice = tts.voices?.firstOrNull { it.locale.language == localeFor(lang).language && !it.isNetworkConnectionRequired }
        return voice != null || tts.voices == null
    }

    /** Fire-and-forget. Returns false if the engine isn't ready or the language is missing. */
    fun speak(text: String, lang: String, flush: Boolean = true, rate: Float = 1.0f): Boolean {
        if (!isReady || text.isBlank()) return false
        val result = tts.setLanguage(localeFor(lang))
        if (result == TextToSpeech.LANG_MISSING_DATA || result == TextToSpeech.LANG_NOT_SUPPORTED) return false
        tts.setSpeechRate(rate)
        val id = "utt-${counter.incrementAndGet()}"
        val queue = if (flush) TextToSpeech.QUEUE_FLUSH else TextToSpeech.QUEUE_ADD
        return tts.speak(text, queue, null, id) == TextToSpeech.SUCCESS
    }

    /** Suspends until the utterance finishes playing. */
    suspend fun speakAndWait(text: String, lang: String, rate: Float = 1.0f): Boolean {
        if (!awaitReady() || text.isBlank()) return false
        val result = tts.setLanguage(localeFor(lang))
        if (result == TextToSpeech.LANG_MISSING_DATA || result == TextToSpeech.LANG_NOT_SUPPORTED) return false
        tts.setSpeechRate(rate)
        val id = "utt-${counter.incrementAndGet()}"
        return suspendCancellableCoroutine { cont ->
            listeners[id] = { if (cont.isActive) cont.resume(true) }
            if (tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, id) != TextToSpeech.SUCCESS) {
                listeners.remove(id)
                cont.resume(false)
            }
            cont.invokeOnCancellation { listeners.remove(id); tts.stop() }
        }
    }

    fun stop() = tts.stop()

    fun shutdown() = tts.shutdown()

    companion object {
        fun localeFor(lang: String): Locale = when (lang) {
            "ko" -> Locale.KOREAN
            "en" -> Locale.US
            "ja" -> Locale.JAPANESE
            "zh" -> Locale.SIMPLIFIED_CHINESE
            "es" -> Locale("es", "ES")
            "vi" -> Locale("vi", "VN")
            "th" -> Locale("th", "TH")
            else -> Locale.forLanguageTag(lang)
        }
    }
}
