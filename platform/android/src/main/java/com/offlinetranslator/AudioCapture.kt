package com.offlinetranslator

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 16 kHz mono PCM16 microphone capture on a dedicated thread. Each buffer (~20 ms) is handed
 * to [onFrames] on that thread — feed it straight into [OfflineTranslator.feedAudio].
 *
 * VOICE_RECOGNITION source disables most OEM post-processing (AGC/compression) which
 * otherwise hurts whisper; the OS NoiseSuppressor is left off because RNNoise runs in-engine.
 */
class AudioCapture(
    private val context: Context,
    private val onFrames: (ShortArray, Int) -> Unit,
    private val onError: (String) -> Unit = {},
) {
    private var record: AudioRecord? = null
    private var thread: Thread? = null
    private val running = AtomicBoolean(false)
    private var aec: AcousticEchoCanceler? = null

    val isRunning: Boolean get() = running.get()

    fun hasPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (running.get()) return true
        if (!hasPermission()) {
            onError("RECORD_AUDIO permission not granted")
            return false
        }
        val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) {
            onError("AudioRecord.getMinBufferSize failed: $minBuf")
            return false
        }
        val bufferBytes = maxOf(minBuf, SAMPLE_RATE * 2 / 5) // ≥ 200 ms of headroom
        val rec = AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT, bufferBytes)
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            onError("AudioRecord failed to initialize")
            rec.release()
            return false
        }
        if (AcousticEchoCanceler.isAvailable()) aec = AcousticEchoCanceler.create(rec.audioSessionId)?.apply { enabled = true }
        if (NoiseSuppressor.isAvailable()) NoiseSuppressor.create(rec.audioSessionId)?.enabled = false

        record = rec
        running.set(true)
        thread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            val chunk = ShortArray(CHUNK_SAMPLES)
            rec.startRecording()
            try {
                while (running.get()) {
                    val n = rec.read(chunk, 0, chunk.size, AudioRecord.READ_BLOCKING)
                    if (n > 0) onFrames(chunk, n)
                    else if (n < 0) { onError("AudioRecord.read error $n"); break }
                }
            } finally {
                runCatching { rec.stop() }
            }
        }, "translator-audio").also { it.start() }
        return true
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        thread?.join(1000)
        thread = null
        aec?.release()
        aec = null
        record?.release()
        record = null
    }

    companion object {
        const val SAMPLE_RATE = 16_000
        const val CHUNK_SAMPLES = 320 // 20 ms
    }
}
