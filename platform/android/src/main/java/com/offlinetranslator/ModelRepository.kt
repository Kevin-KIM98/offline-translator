package com.offlinetranslator

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.ProducerScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.Closeable
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.ZipInputStream
import kotlin.coroutines.coroutineContext

enum class ModelState { READY, MISSING, CORRUPT, UPDATE_AVAILABLE, UNVERIFIED;
    companion object {
        fun parse(s: String) = when (s) {
            "ready" -> READY; "missing" -> MISSING; "corrupt" -> CORRUPT
            "update_available" -> UPDATE_AVAILABLE; else -> UNVERIFIED
        }
    }
}

data class DownloadItem(val url: String, val filename: String, val sizeBytes: Long, val sha256: String, val isArchive: Boolean)

data class ModelStatus(
    val id: String,
    val kind: String,
    val pair: String,
    /** Human-facing name from the manifest, e.g. "Qwen2.5 3B" (LLM entries). */
    val label: String,
    val version: String,
    val state: ModelState,
    val detail: String,
    val installPath: String,
    val totalBytes: Long,
    val downloads: List<DownloadItem>,
) {
    val needsDownload: Boolean get() = state != ModelState.READY

    companion object {
        fun fromJson(o: JSONObject): ModelStatus {
            val dl = o.optJSONArray("downloads")
            return ModelStatus(
                id = o.getString("id"),
                kind = o.optString("kind"),
                pair = o.optString("pair"),
                label = o.optString("label"),
                version = o.optString("version"),
                state = ModelState.parse(o.optString("state")),
                detail = o.optString("detail"),
                installPath = o.optString("install_path"),
                totalBytes = o.optLong("total_bytes"),
                downloads = if (dl == null) emptyList() else List(dl.length()) { i ->
                    val d = dl.getJSONObject(i)
                    DownloadItem(d.optString("url"), d.optString("filename"), d.optLong("size_bytes"), d.optString("sha256"), d.optBoolean("archive"))
                },
            )
        }
    }
}

sealed class InstallEvent {
    data class Started(val id: String, val totalBytes: Long) : InstallEvent()
    data class Downloading(val id: String, val file: String, val bytesDone: Long, val bytesTotal: Long) : InstallEvent()
    data class Verifying(val id: String, val bytesDone: Long, val bytesTotal: Long) : InstallEvent()
    data class Installed(val id: String) : InstallEvent()
    data class Failed(val id: String, val reason: String) : InstallEvent()
    data class AllDone(val failed: List<String>) : InstallEvent()
}

/**
 * Model asset lifecycle on Android:
 *   1. [refreshManifest] fetches manifest.json (falls back to the cached copy / bundled asset).
 *   2. [status] / [statusForLanguages] report what is missing or outdated.
 *   3. [installAll] downloads into the native staging dir (resumable), verifies SHA-256 via the
 *      C++ core, extracts zips (java.util.zip) and installs atomically.
 *
 * Networking lives here so the app can swap in OkHttp / WorkManager without touching C++.
 */
class ModelRepository(
    private val context: Context,
    val modelsRoot: File = File(context.filesDir, "models"),
    private val manifestUrl: String? = DEFAULT_MANIFEST_URL,
    private val bundledManifestAsset: String? = "manifest.json",
) : Closeable {

    companion object {
        /** Default manifest: models hosted on the project's GitHub releases. Override to self-host. */
        const val DEFAULT_MANIFEST_URL = "https://raw.githubusercontent.com/Kevin-KIM98/offline-translator/main/assets/manifest.json"

        /** Extra attempts per file after a transient failure; the pause doubles from [RETRY_DELAY_MS]. */
        private const val MAX_DOWNLOAD_RETRIES = 4
        private const val RETRY_DELAY_MS = 1_000L
    }

    private var handle: Long = NativeBridge.mmCreate(modelsRoot.absolutePath)

    init {
        if (handle == 0L) throw TranslatorException("model manager init failed: ${NativeBridge.lastGlobalError()}")
        if (!NativeBridge.mmLoadCachedManifest(handle)) loadBundledManifest()
    }

    private fun h(): Long = handle.takeIf { it != 0L } ?: throw IllegalStateException("repository closed")

    val lastError: String get() = NativeBridge.mmLastError(h())
    val manifestVersion: String get() = NativeBridge.mmManifestVersion(h())
    val sttModelPath: String get() = NativeBridge.mmSttModelPath(h())
    val nmtRootDir: String get() = NativeBridge.mmNmtRootDir(h())
    /** Path the manifest's LLM installs to (may not exist yet); empty when the manifest has no LLM. */
    val llmModelPath: String get() = NativeBridge.mmLlmModelPath(h())

    /** Install path of the LLM `llmId` names; of the manifest's default when null or unknown. */
    fun llmModelPath(llmId: String?): String = NativeBridge.mmLlmModelPathFor(h(), llmId ?: "")

    /** Every LLM the manifest offers, default first. `label` names it, `state` says whether it is installed. */
    fun llmOptions(): List<ModelStatus> = status().filter { it.kind == "llm" }

    /** Install path of the speech model `sttId` names; of the manifest's default when null or unknown. */
    fun sttModelPath(sttId: String?): String = NativeBridge.mmSttModelPathFor(h(), sttId ?: "")

    /** Every speech (whisper) model the manifest offers, default first. */
    fun sttOptions(): List<ModelStatus> = status().filter { it.kind == "stt" }

    fun pipelineConfig(
        nThreads: Int? = null,
        backend: TranslationBackend = TranslationBackend.AUTO,
        llmId: String? = null,
        sttId: String? = null,
        useGpu: Boolean = false,
    ): PipelineConfig {
        val llm = llmModelPath(llmId).takeIf { it.isNotEmpty() && File(it).isFile }
        return PipelineConfig(
            whisperModelPath = sttModelPath(sttId).ifEmpty { null },
            nmtRootDir = nmtRootDir,
            llmModelPath = llm,
            backend = backend,
            useGpu = useGpu,
            nThreads = nThreads ?: Runtime.getRuntime().availableProcessors().coerceIn(2, 6),
        )
    }

    private fun loadBundledManifest(): Boolean {
        val asset = bundledManifestAsset ?: return false
        return runCatching {
            val json = context.assets.open(asset).bufferedReader().use { it.readText() }
            NativeBridge.mmLoadManifestJson(h(), json).also { if (it) NativeBridge.mmSaveManifest(h()) }
        }.getOrDefault(false)
    }

    /**
     * Downloads the latest manifest; returns true if a manifest is loaded afterwards. A failed
     * fetch is retried a few times (the host answers 5xx now and then) before falling back to the
     * cached or bundled copy.
     */
    suspend fun refreshManifest(): Boolean = withContext(Dispatchers.IO) {
        val url = manifestUrl ?: return@withContext hasManifest()
        // With a manifest already in hand a failed fetch costs nothing, so do not make every
        // offline start wait; only the first run, where no manifest means no download at all,
        // is worth a couple of retries.
        val attempts = if (hasManifest()) 1 else 3
        var json: String? = null
        for (attempt in 0 until attempts) {
            json = runCatching { URL(url).openStream().bufferedReader().use { it.readText() } }.getOrNull()
            if (json != null) break
            if (attempt < attempts - 1) delay(RETRY_DELAY_MS shl attempt)
        }
        if (json == null) return@withContext hasManifest()
        val ok = NativeBridge.mmLoadManifestJson(h(), json)
        if (ok) NativeBridge.mmSaveManifest(h())
        ok || hasManifest()
    }

    fun hasManifest(): Boolean = manifestVersion.isNotEmpty()

    /** deepVerify re-hashes installed files — call from Dispatchers.IO. */
    fun status(deepVerify: Boolean = false): List<ModelStatus> = parseStatus(NativeBridge.mmStatusJson(h(), deepVerify))

    /**
     * Models the given languages need. `llmId` picks which LLM counts and `sttId` which speech
     * model; null means the manifest's default.
     */
    fun statusForLanguages(
        langs: Collection<String>,
        deepVerify: Boolean = false,
        llmMode: LlmMode = LlmMode.IF_NEEDED,
        llmId: String? = null,
        sttId: String? = null,
    ): List<ModelStatus> =
        parseStatus(NativeBridge.mmStatusForLanguagesJson2(h(), langs.joinToString(","), deepVerify, llmMode.native, llmId ?: "", sttId ?: ""))

    private fun parseStatus(json: String): List<ModelStatus> {
        val arr = JSONObject(json).optJSONArray("models") ?: return emptyList()
        return List(arr.length()) { ModelStatus.fromJson(arr.getJSONObject(it)) }
    }

    fun pendingBytes(langs: Collection<String>? = null): Long =
        (if (langs == null) status() else statusForLanguages(langs)).filter { it.needsDownload }.sumOf { it.totalBytes }

    fun remove(id: String): Boolean = NativeBridge.mmRemove(h(), id)

    /** Free space on the models volume, for a pre-flight check before [installAll]. */
    fun freeBytes(): Long = modelsRoot.apply { mkdirs() }.usableSpace

    /**
     * Installs every model in [models] that needs download. Emits progress; per-model failures
     * are reported as [InstallEvent.Failed] and summarised in [InstallEvent.AllDone].
     */
    fun installAll(models: List<ModelStatus>): Flow<InstallEvent> = channelFlow {
        val failed = ArrayList<String>()
        for (m in models.filter { it.needsDownload }) {
            send(InstallEvent.Started(m.id, m.totalBytes))
            val error = try {
                installOne(m, this)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                e.message ?: e.toString()
            }
            if (error == null) {
                send(InstallEvent.Installed(m.id))
            } else {
                failed += m.id
                send(InstallEvent.Failed(m.id, error))
            }
        }
        send(InstallEvent.AllDone(failed))
    }.flowOn(Dispatchers.IO)

    /** Returns null on success, else an error message. */
    private suspend fun installOne(m: ModelStatus, scope: ProducerScope<InstallEvent>): String? {
        val staging = File(NativeBridge.mmStagingDir(h(), m.id))
        var downloaded = 0L
        for (d in m.downloads) {
            val target = File(staging, d.filename)
            val base = downloaded
            download(d.url, target, d.sizeBytes) { done, total ->
                scope.send(InstallEvent.Downloading(m.id, d.filename, base + done, if (m.totalBytes > 0) m.totalBytes else total))
            }
            downloaded += d.sizeBytes

            if (d.isArchive) {
                // Archives must be verified BEFORE extraction; the core has no hashes for extracted files.
                // Progress callbacks arrive synchronously on this thread → non-suspending trySend.
                val v = NativeBridge.mmVerifyFile(h(), target.absolutePath, d.sha256, d.sizeBytes) { done, total ->
                    scope.trySend(InstallEvent.Verifying(m.id, done, total))
                    scope.isActive
                }
                if (v != 1) {
                    target.delete()
                    return "verify failed for ${d.filename}: $lastError"
                }
                extractZip(target, staging)
                target.delete()
            }
        }
        // Non-archive files are (re)hashed inside install() — verifyHashes = true.
        val ok = NativeBridge.mmInstall(h(), m.id, staging.absolutePath, true) { done, total ->
            scope.trySend(InstallEvent.Verifying(m.id, done, total))
            scope.isActive
        }
        if (!ok) {
            val reason = lastError
            NativeBridge.mmClearStaging(h(), m.id)
            return "install failed: $reason"
        }
        return null
    }

    /**
     * Resumable HTTP download (Range requests) into `target` via a `.part` file. A transient
     * failure — a 5xx from the release host, 408/429, a timeout or a cut connection — is retried
     * with a growing pause, resuming from the bytes already on disk; a 404 or another permanent
     * answer fails at once. Without this, one hiccup out of the hundreds of files a full download
     * fetches ended the whole run (seen as "HTTP 500 for .../nmt_en-es_target.spm").
     */
    private suspend fun download(url: String, target: File, expectedSize: Long, progress: suspend (Long, Long) -> Unit) {
        target.parentFile?.mkdirs()
        if (target.exists() && expectedSize > 0 && target.length() == expectedSize) {
            progress(expectedSize, expectedSize)
            return
        }
        val part = File(target.path + ".part")
        var attempt = 0
        while (true) {
            try {
                fetch(url, part, expectedSize, progress)
                break
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                if (attempt >= MAX_DOWNLOAD_RETRIES || !isTransient(e)) throw e
                delay(RETRY_DELAY_MS shl attempt)
                attempt++
            }
        }
        if (target.exists()) target.delete()
        if (!part.renameTo(target)) throw TranslatorException("cannot finalize ${target.name}")
    }

    /** One attempt: appends to `part` until the file is complete, or throws. */
    private suspend fun fetch(url: String, part: File, expectedSize: Long, progress: suspend (Long, Long) -> Unit) {
        var offset = if (part.exists()) part.length() else 0L
        if (expectedSize in 1..offset) { part.delete(); offset = 0 } // stale or corrupt partial

        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15_000
            readTimeout = 30_000
            if (offset > 0) setRequestProperty("Range", "bytes=$offset-")
        }
        try {
            val code = conn.responseCode
            val append = when (code) {
                HttpURLConnection.HTTP_PARTIAL -> true
                HttpURLConnection.HTTP_OK -> { offset = 0; false }
                else -> throw HttpStatusException(code, url)
            }
            val total = if (expectedSize > 0) expectedSize else conn.contentLengthLong.let { if (it > 0) it + offset else -1L }
            var done = offset
            conn.inputStream.use { input ->
                FileOutputStream(part, append).use { out ->
                    val buf = ByteArray(256 * 1024)
                    var lastReport = 0L
                    while (true) {
                        coroutineContext.ensureActive()
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (done - lastReport >= 512 * 1024 || done == total) {
                            lastReport = done
                            progress(done, total)
                        }
                    }
                    out.fd.sync()
                }
            }
        } finally {
            conn.disconnect()
        }
        // A connection cut short leaves a shorter file; the next attempt resumes from here.
        if (expectedSize > 0 && part.length() != expectedSize) {
            throw IOException("incomplete download: ${part.length()} of $expectedSize bytes")
        }
    }

    /** A failure worth another attempt: the host's problem, not the file's. */
    private fun isTransient(e: Exception): Boolean = when (e) {
        is HttpStatusException -> e.code >= 500 || e.code == 408 || e.code == 429
        is IOException -> true   // timeouts, resets, DNS, a connection cut mid-file
        else -> false
    }

    private class HttpStatusException(val code: Int, url: String) : IOException("HTTP $code for $url")

    private fun extractZip(zip: File, into: File) {
        ZipInputStream(zip.inputStream().buffered()).use { zin ->
            val canonicalRoot = into.canonicalPath + File.separator
            while (true) {
                val entry = zin.nextEntry ?: break
                val out = File(into, entry.name)
                if (!out.canonicalPath.startsWith(canonicalRoot)) throw TranslatorException("zip slip: ${entry.name}")
                if (entry.isDirectory) { out.mkdirs(); continue }
                out.parentFile?.mkdirs()
                FileOutputStream(out).use { zin.copyTo(it) }
                zin.closeEntry()
            }
        }
    }

    override fun close() {
        if (handle != 0L) {
            NativeBridge.mmDestroy(handle)
            handle = 0L
        }
    }
}
