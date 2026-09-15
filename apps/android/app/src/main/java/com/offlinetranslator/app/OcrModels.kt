package com.offlinetranslator.app

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import kotlin.coroutines.coroutineContext

/** One Tesseract language file, as the model manifest's `ocr` section lists it. */
data class OcrModel(
    /** Engine language code, e.g. "ko". */
    val lang: String,
    /** Tesseract's code for the same language, e.g. "kor" — also the file's stem. */
    val tessLang: String,
    val filename: String,
    val sizeBytes: Long,
    val sha256: String,
    val url: String,
)

/**
 * Text-recognition data for the camera: Tesseract `tessdata_fast` files, one per language,
 * listed in the manifest's `ocr` section and hosted with the other models. They live under
 * files/ocr/tessdata, outside the engine's model store, because the engine knows nothing about
 * them: the app runs Tesseract itself. Each file is a few megabytes, so it is fetched the first
 * time a language is used with the camera rather than during the first-run setup.
 */
class OcrModels(context: Context) {
    private companion object {
        /** Extra attempts after a transient failure; the pause doubles from [RETRY_DELAY_MS]. */
        const val MAX_RETRIES = 4
        const val RETRY_DELAY_MS = 1_000L
    }

    /** Handed to Tesseract; it looks for `tessdata/<lang>.traineddata` below it. */
    val root: File = File(context.filesDir, "ocr")
    private val tessdata = File(root, "tessdata")

    /** The manifest the engine cached last (ModelRepository saves every one it loads). */
    private val cachedManifest = File(context.filesDir, "models/manifest.json")

    /** Every language the manifest offers text recognition for; empty until a manifest with an `ocr` section was seen. */
    fun catalog(): List<OcrModel> {
        val text = runCatching { cachedManifest.readText() }.getOrNull() ?: return emptyList()
        val m = runCatching { JSONObject(text) }.getOrNull() ?: return emptyList()
        val base = m.optString("base_url")
        val arr = m.optJSONArray("ocr") ?: return emptyList()
        return List(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            val url = o.optString("download_url")
            OcrModel(
                lang = o.optString("lang"),
                tessLang = o.optString("tess_lang", o.optString("filename").substringBeforeLast('.')),
                filename = o.optString("filename"),
                sizeBytes = o.optLong("size_bytes"),
                sha256 = o.optString("sha256"),
                url = if (url.startsWith("http")) url else base + url,
            )
        }.filter { it.lang.isNotEmpty() && it.filename.isNotEmpty() }
    }

    fun file(m: OcrModel): File = File(tessdata, m.filename)

    fun isInstalled(m: OcrModel): Boolean = file(m).let { it.isFile && (m.sizeBytes <= 0 || it.length() == m.sizeBytes) }

    fun installed(catalog: List<OcrModel> = catalog()): List<OcrModel> = catalog.filter { isInstalled(it) }

    fun remove(m: OcrModel): Boolean = file(m).delete()

    /**
     * Downloads one language file, resuming a partial one, and keeps it only when its SHA-256
     * matches the manifest. [progress] gets (bytes done, bytes total). A transient failure (5xx
     * from the release host, 408/429, a timeout, a cut connection) is retried with a growing
     * pause, resuming from what is already on disk.
     */
    suspend fun install(m: OcrModel, progress: suspend (Long, Long) -> Unit) = withContext(Dispatchers.IO) {
        tessdata.mkdirs()
        val target = file(m)
        if (isInstalled(m)) {
            progress(m.sizeBytes, m.sizeBytes)
            return@withContext
        }
        val part = File(target.path + ".part")
        var attempt = 0
        while (true) {
            try {
                fetch(m, part, progress)
                break
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                if (attempt >= MAX_RETRIES || !isTransient(e)) throw e   // the partial file stays; a later run resumes
                delay(RETRY_DELAY_MS shl attempt)
                attempt++
            }
        }
        if (m.sha256.isNotEmpty() && !sha256Of(part).equals(m.sha256, ignoreCase = true)) {
            part.delete()
            throw IllegalStateException("checksum mismatch for ${m.filename}")
        }
        if (target.exists()) target.delete()
        if (!part.renameTo(target)) throw IllegalStateException("cannot finalize ${m.filename}")
    }

    /** One attempt: appends to `part` until the file is complete, or throws. */
    private suspend fun fetch(m: OcrModel, part: File, progress: suspend (Long, Long) -> Unit) {
        var offset = if (part.exists()) part.length() else 0L
        if (m.sizeBytes in 1..offset) { part.delete(); offset = 0 }

        val conn = (URL(m.url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15_000
            readTimeout = 30_000
            if (offset > 0) setRequestProperty("Range", "bytes=$offset-")
        }
        try {
            val append = when (val code = conn.responseCode) {
                HttpURLConnection.HTTP_PARTIAL -> true
                HttpURLConnection.HTTP_OK -> { offset = 0; false }
                else -> throw HttpStatusException(code, m.url)
            }
            val total = if (m.sizeBytes > 0) m.sizeBytes else conn.contentLengthLong.let { if (it > 0) it + offset else -1L }
            var done = offset
            conn.inputStream.use { input ->
                FileOutputStream(part, append).use { out ->
                    val buf = ByteArray(128 * 1024)
                    while (true) {
                        coroutineContext.ensureActive()
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        progress(done, total)
                    }
                    out.fd.sync()
                }
            }
        } finally {
            conn.disconnect()
        }
        if (m.sizeBytes > 0 && part.length() != m.sizeBytes) {
            throw IOException("incomplete download: ${part.length()} of ${m.sizeBytes} bytes")
        }
    }

    private fun isTransient(e: Exception): Boolean = when (e) {
        is HttpStatusException -> e.code >= 500 || e.code == 408 || e.code == 429
        is IOException -> true
        else -> false
    }

    private class HttpStatusException(val code: Int, url: String) : IOException("HTTP $code for $url")

    private fun sha256Of(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        f.inputStream().use { input ->
            val buf = ByteArray(256 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }
}
