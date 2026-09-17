package com.offlinetranslator.app

import android.app.Application
import android.graphics.Bitmap
import android.net.Uri
import android.os.SystemClock
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.offlinetranslator.InstallEvent
import com.offlinetranslator.LlmMode
import com.offlinetranslator.ModelRepository
import com.offlinetranslator.ModelStatus
import com.offlinetranslator.OfflineTranslator
import com.offlinetranslator.TranslationBackend
import com.offlinetranslator.TranslationResult
import com.offlinetranslator.TranslatorSession
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.Executors

/** Where the app is in its lifecycle: models first, then a live session. */
sealed interface Phase {
    /** Reading the manifest and checking what is installed. */
    data object Checking : Phase

    /**
     * Models are missing; [pending] is what the chosen languages still need — or, with
     * [everything], every model the manifest offers plus the text-recognition files for photo
     * translation ([ocrPending]), so no later choice needs a download.
     */
    data class Setup(
        val pending: List<ModelStatus>,
        val error: String? = null,
        val everything: Boolean = false,
        val ocrPending: List<OcrModel> = emptyList(),
    ) : Phase {
        val totalBytes: Long get() = pending.sumOf { it.totalBytes } + ocrPending.sumOf { it.sizeBytes }
    }

    data class Downloading(
        val label: String,
        val bytesDone: Long,
        val bytesTotal: Long,
        val verifying: Boolean = false,
    ) : Phase

    /** Loading whisper and the translation models into memory. */
    data object Opening : Phase

    data object Ready : Phase

    data class Fatal(val message: String) : Phase
}

/** One line of the conversation. */
data class Turn(
    val id: Long,
    val side: Side,
    val sourceText: String,
    val sourceLang: String,
    val translatedText: String,
    val targetLang: String,
    val route: List<String>,
    val totalMs: Double,
    /** The source text was read from a photo rather than spoken or typed. */
    val fromImage: Boolean = false,
)

/**
 * A piece of text read from a photo and its translation: blank when that piece failed, or when it
 * is [kept] as it is because it is a code, a number or a price rather than language.
 */
data class PhotoText(val region: OcrRegion, val translation: String, val kept: Boolean = false)

/** What the camera screen shows. */
sealed interface CameraPhase {
    data object Preview : CameraPhase

    /** A picked photo is being opened and scaled. */
    data object Loading : CameraPhase

    /** Reading the photo ([translating] false) or translating what was read (true). */
    data class Working(val image: Bitmap, val translating: Boolean) : CameraPhase

    /**
     * Every piece of text in [image] with its translation, to be painted where the text stands;
     * [ocrMs] reading the photo and [nmtMs] translating it.
     */
    data class Result(val image: Bitmap, val turn: Turn, val texts: List<PhotoText>, val ocrMs: Double, val nmtMs: Double) : CameraPhase

    /**
     * Live mode: the viewfinder with the last frame's translations painted over the scene. The
     * boxes of [texts] are in the pixels of a frame [frameWidth] × [frameHeight]; [ocrMs] and
     * [nmtMs] are what that frame cost, shown on screen because the phone's real speed can be
     * read nowhere else. [pending] counts the pieces still waiting for the engine.
     */
    data class Live(
        val texts: List<PhotoText> = emptyList(),
        val frameWidth: Int = 0,
        val frameHeight: Int = 0,
        val ocrMs: Double = 0.0,
        val nmtMs: Double = 0.0,
        val pending: Int = 0,
    ) : CameraPhase

    data class Failed(val image: Bitmap?, val message: String) : CameraPhase
}

/** Texts live mode keeps translated; beyond this the least recently seen one goes. */
private const val LIVE_CACHE = 200

/** Pieces of text one live frame may hand to the engine; the rest wait for a later frame. */
private const val LIVE_MAX_NEW = 4

/** A text-recognition file on its way in. */
data class OcrDownload(val lang: String, val bytesDone: Long, val bytesTotal: Long)

data class UiState(
    val phase: Phase = Phase.Checking,
    val langA: String = "ko",
    val langB: String = "en",
    val turns: List<Turn> = emptyList(),
    val listening: Side? = null,
    val working: Boolean = false,
    val speakingTurn: Long? = null,
    val level: Float = 0f,
    val speak: Boolean = true,
    val handsFree: Boolean = false,
    val speechRate: Float = 1.0f,
    val backend: TranslationBackend = TranslationBackend.AUTO,
    val message: String? = null,
    val installed: List<ModelStatus> = emptyList(),
    /**
     * The LLM in use (picked by [ModelPolicy] from the languages when [llmAuto], else the user's
     * choice), null for the manifest's default; every LLM the manifest offers.
     */
    val llmId: String? = null,
    val llmAuto: Boolean = true,
    val llmOptions: List<ModelStatus> = emptyList(),
    /** The speech model in use (as [llmId]); every whisper model the manifest offers. */
    val sttId: String? = null,
    val sttAuto: Boolean = true,
    val sttOptions: List<ModelStatus> = emptyList(),
    /** Whisper on the GPU (experimental). */
    val useGpu: Boolean = false,
    /** Camera translation: the screen's phase, the text-recognition files the manifest offers and the ones on the phone. */
    val camera: CameraPhase = CameraPhase.Preview,
    val ocrCatalog: List<OcrModel> = emptyList(),
    val ocrInstalled: Set<String> = emptySet(),
    val ocrDownload: OcrDownload? = null,
) {
    val langs: List<String> get() = listOf(langA, langB)
}

class MainViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs = Prefs(app)
    private val _state = MutableStateFlow(
        UiState(
            langA = prefs.langA,
            langB = prefs.langB,
            speak = prefs.speak,
            speechRate = prefs.speechRate,
            backend = prefs.backend,
            llmId = if (prefs.llmAuto) ModelPolicy.llmFor(listOf(prefs.langA, prefs.langB), prefs.roomy) else prefs.llmId,
            llmAuto = prefs.llmAuto,
            sttId = if (prefs.sttAuto) ModelPolicy.sttFor(listOf(prefs.langA, prefs.langB), prefs.roomy) else prefs.sttId,
            sttAuto = prefs.sttAuto,
            useGpu = prefs.useGpu,
        )
    )
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var repo: ModelRepository? = null
    private var session: TranslatorSession? = null
    private var downloadJob: Job? = null
    private var busyWatcher: Job? = null
    private var ocrJob: Job? = null
    private val ocr = OcrModels(app)

    /**
     * Live mode runs its recogniser on one thread of its own: Tesseract is opened, read and
     * recycled in that order, and a frame is never read while the recogniser is being closed.
     */
    private val liveExecutor = Executors.newSingleThreadExecutor()
    private val liveDispatcher = liveExecutor.asCoroutineDispatcher()
    private var liveJob: Job? = null

    /** The open recogniser and its language; opened, used and closed on the live thread only. */
    private var liveReader: LiveOcr? = null
    private var liveLang: String? = null
    private var liveSide = Side.A
    private var liveResume = false

    /** Whether a frame is being read or translated; the viewfinder's other frames are dropped. */
    @Volatile
    private var liveBusy = false

    /**
     * What live mode has already translated, so a sign stays painted while the phone moves and is
     * sent to the engine once. Least-recently-seen text goes first.
     */
    private val liveCache = object : LinkedHashMap<String, String>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, String>): Boolean = size > LIVE_CACHE
    }
    private var nextTurnId = 1L

    /** Side the user is currently holding the talk button for, in manual mode. */
    private var pendingSide: Side = Side.A

    init {
        boot()
    }

    // ---------------------------------------------------------------- startup

    fun boot() {
        // A launch that never got past opening the engine with the GPU on: the driver took the
        // process down. Back to the CPU, and say so, rather than dying again.
        if (prefs.gpuTrialPending && prefs.useGpu) {
            prefs.useGpu = false
            prefs.gpuTrialPending = false
            _state.update { it.copy(useGpu = false, message = str(R.string.gpu_disabled_after_crash)) }
        }
        // The languages may have changed since the models were picked.
        _state.update {
            it.copy(
                phase = Phase.Checking,
                sttId = if (it.sttAuto) ModelPolicy.sttFor(it.langs, prefs.roomy) else it.sttId,
                llmId = if (it.llmAuto) ModelPolicy.llmFor(it.langs, prefs.roomy) else it.llmId,
            )
        }
        viewModelScope.launch {
            val r = runCatching {
                withContext(Dispatchers.IO) {
                    val repository = repo ?: ModelRepository(getApplication<Application>()).also { repo = it }
                    repository.refreshManifest()
                    if (!repository.hasManifest()) error(str(R.string.err_no_manifest))
                    repository.statusForLanguages(_state.value.langs, llmMode = llmMode(), llmId = _state.value.llmId, sttId = _state.value.sttId)
                }
            }
            r.onFailure { e -> _state.update { it.copy(phase = Phase.Fatal(e.message ?: str(R.string.err_init))) } }
            r.onSuccess { status ->
                refreshInstalled()
                val pending = status.filter { it.needsDownload }
                if (pending.isEmpty()) openSession() else _state.update { it.copy(phase = Phase.Setup(pending)) }
            }
        }
    }

    private fun llmMode(): LlmMode =
        if (_state.value.backend == TranslationBackend.LLM) LlmMode.ALWAYS else LlmMode.IF_NEEDED

    fun download() {
        val setup = _state.value.phase as? Phase.Setup ?: return
        val pending = setup.pending
        val ocrPending = setup.ocrPending
        if (pending.isEmpty() && ocrPending.isEmpty()) return
        val total = setup.totalBytes
        downloadJob?.cancel()
        downloadJob = viewModelScope.launch {
            val repository = repo ?: return@launch
            val free = withContext(Dispatchers.IO) { repository.freeBytes() }
            if (free < total + 50L * 1024 * 1024) {
                _state.update {
                    it.copy(phase = setup.copy(error = str(R.string.err_storage, mb(total))))
                }
                return@launch
            }
            val firstLabel = pending.firstOrNull()?.let { label(it) } ?: ocrLabel(ocrPending.first())
            _state.update { it.copy(phase = Phase.Downloading(firstLabel, 0, total)) }
            var completed = 0L
            var failure: String? = null
            var failures = 0
            if (pending.isNotEmpty()) runCatching {
                repository.installAll(pending).collect { ev ->
                    when (ev) {
                        is InstallEvent.Started ->
                            _state.update { it.copy(phase = Phase.Downloading(labelOf(pending, ev.id), completed, total)) }
                        is InstallEvent.Downloading ->
                            _state.update { it.copy(phase = Phase.Downloading(labelOf(pending, ev.id), completed + ev.bytesDone, total)) }
                        is InstallEvent.Verifying ->
                            _state.update { it.copy(phase = Phase.Downloading(labelOf(pending, ev.id), completed, total, verifying = true)) }
                        is InstallEvent.Installed ->
                            completed += pending.firstOrNull { it.id == ev.id }?.totalBytes ?: 0L
                        is InstallEvent.Failed -> {
                            failures++
                            if (failure == null) failure = "${labelOf(pending, ev.id)}: ${ev.reason}"
                        }
                        is InstallEvent.AllDone -> {}
                    }
                }
            }.onFailure { e ->
                failures++
                if (failure == null) failure = e.message ?: str(R.string.err_download_failed)
            }

            // The text-recognition files ("download everything"): a few MB each, after the models.
            // One file failing does not stop the rest — whatever arrives is kept, and the setup
            // screen afterwards lists only what is still missing.
            for (model in ocrPending) {
                val name = ocrLabel(model)
                _state.update { it.copy(phase = Phase.Downloading(name, completed, total)) }
                runCatching {
                    ocr.install(model) { done, _ ->
                        _state.update { it.copy(phase = Phase.Downloading(name, completed + done, total)) }
                    }
                }.onFailure { e ->
                    failures++
                    if (failure == null) failure = "$name: ${e.message ?: str(R.string.err_download_failed)}"
                }
                completed += model.sizeBytes
            }
            refreshOcr()

            val err = failure
            if (err == null) {
                prefs.setupDone = true
                openSession()
                return@launch
            }
            // Everything that did arrive is installed: ask the core again so pressing the button
            // once more fetches only what is left instead of the whole set.
            val left = remaining(setup)
            val message =
                if (failures > 1) str(R.string.err_download_partial, err, failures - 1)
                else str(R.string.err_download_one, err)
            if (left.pending.isEmpty() && left.ocrPending.isEmpty()) {
                prefs.setupDone = true
                openSession()
            } else {
                _state.update { it.copy(phase = left.copy(error = message)) }
            }
        }
    }

    private fun ocrLabel(m: OcrModel): String = str(R.string.ocr_model_title, Lang.of(m.lang).name)

    /** What [setup] still needs after a download run, in the same shape the setup screen wants. */
    private suspend fun remaining(setup: Phase.Setup): Phase.Setup = withContext(Dispatchers.IO) {
        val status = runCatching {
            if (setup.everything) repo?.status()
            else repo?.statusForLanguages(_state.value.langs, llmMode = llmMode(), llmId = _state.value.llmId, sttId = _state.value.sttId)
        }.getOrNull()
        // Without a fresh status keep the old list: re-downloading beats hiding a missing model.
        val pending = status?.filter { it.needsDownload } ?: setup.pending
        val ocrPending = setup.ocrPending.filterNot { ocr.isInstalled(it) }
        setup.copy(pending = pending, ocrPending = ocrPending, error = null)
    }

    fun cancelDownload() {
        downloadJob?.cancel()
        viewModelScope.launch {
            val pending = withContext(Dispatchers.IO) {
                runCatching { repo?.statusForLanguages(_state.value.langs, llmMode = llmMode(), llmId = _state.value.llmId, sttId = _state.value.sttId)?.filter { it.needsDownload } }
                    .getOrNull()
            } ?: emptyList()
            // Stopping a "download everything" run leaves the chosen languages either complete
            // (back to the conversation) or short of something (the usual setup screen).
            if (pending.isEmpty()) openSession()
            else _state.update { it.copy(phase = Phase.Setup(pending, str(R.string.err_download_stopped))) }
        }
    }

    /**
     * Every model the manifest offers — both speech models, every translation pair, both LLMs —
     * and every text-recognition file for photo translation, in one download, so changing
     * languages or models later never waits for the network. The setup screen lists them with
     * the total size and asks before starting.
     */
    fun downloadEverything() {
        viewModelScope.launch {
            val pending = withContext(Dispatchers.IO) { runCatching { repo?.status() }.getOrNull() }
                ?.filter { it.needsDownload } ?: emptyList()
            val ocrPending = withContext(Dispatchers.IO) {
                val catalog = ocr.catalog()
                catalog.filterNot { ocr.isInstalled(it) }
            }
            if (pending.isEmpty() && ocrPending.isEmpty()) {
                showMessage(str(R.string.all_models_installed))
                return@launch
            }
            pauseMic()
            _state.update {
                it.copy(phase = Phase.Setup(pending, everything = true, ocrPending = ocrPending), handsFree = false, listening = null)
            }
        }
    }

    private fun openSession() {
        viewModelScope.launch {
            _state.update { it.copy(phase = Phase.Opening) }
            val old = session
            session = null
            withContext(Dispatchers.Default) { runCatching { old?.close() } }

            val built = runCatching {
                withContext(Dispatchers.Default) {
                    val repository = repo ?: error("model repository closed")
                    val useGpu = _state.value.useGpu
                    val config = repository.pipelineConfig(
                        backend = _state.value.backend, llmId = _state.value.llmId, sttId = _state.value.sttId, useGpu = useGpu,
                    )
                    if (useGpu) prefs.gpuTrialPending = true
                    // The app drives TTS itself so a line can be replayed and muted mid-sentence.
                    TranslatorSession(getApplication<Application>(), config, speakResults = false).also {
                        if (useGpu) prefs.gpuTrialPending = false
                    }
                }
            }
            built.onFailure { e -> _state.update { it.copy(phase = Phase.Fatal(e.message ?: str(R.string.err_engine_open))) } }
            built.onSuccess { s ->
                s.speechRate = _state.value.speechRate
                s.onResult = { r -> onEngineResult(r) }
                // Silence produces no result; without this the "translating" bar never clears.
                s.onNoSpeech = { _state.update { it.copy(working = false) } }
                s.onError = { msg -> _state.update { it.copy(message = msg, working = false) } }
                s.onAudioLevel = { lv -> _state.update { it.copy(level = lv) } }
                session = s
                _state.update { it.copy(phase = Phase.Ready) }
                refreshInstalled()
                if (_state.value.handsFree) startHandsFree()
            }
        }
    }

    // ------------------------------------------------------------ conversation

    private fun onEngineResult(r: TranslationResult) {
        val s = _state.value
        if (!r.ok) {
            _state.update { it.copy(message = r.error ?: str(R.string.err_translate), working = false) }
            return
        }
        if (r.sourceText.isBlank()) {
            _state.update { it.copy(working = false) }
            return
        }
        // Conversation mode: the engine detected the language and chose the direction itself
        // (B's language goes to A, anything else goes to B); the side follows the detection.
        val side = if (s.handsFree) (if (r.sourceLang == s.langB && s.langA != s.langB) Side.B else Side.A) else pendingSide
        addTurn(r, side)
    }

    private fun addTurn(r: TranslationResult, side: Side) {
        val turn = Turn(
            id = nextTurnId++,
            side = side,
            sourceText = r.sourceText,
            sourceLang = r.sourceLang,
            translatedText = r.translatedText,
            targetLang = r.targetLang,
            route = r.route,
            totalMs = r.totalMs,
        )
        _state.update { it.copy(turns = it.turns + turn, working = false) }
        if (_state.value.speak) speakTurn(turn)
    }

    fun speakTurn(turn: Turn) {
        viewModelScope.launch {
            _state.update { it.copy(speakingTurn = turn.id) }
            session?.speak(turn.translatedText, turn.targetLang)
            _state.update { if (it.speakingTurn == turn.id) it.copy(speakingTurn = null) else it }
        }
    }

    fun stopSpeaking() {
        session?.stopSpeaking()
        _state.update { it.copy(speakingTurn = null) }
    }

    /** Manual mode: talk button pressed for [side]. */
    fun startTalking(side: Side) {
        val s = _state.value
        if (s.phase != Phase.Ready || s.handsFree) return
        pendingSide = side
        session?.stopSpeaking()
        val src = if (side == Side.A) s.langA else s.langB
        val tgt = if (side == Side.A) s.langB else s.langA
        if (session?.start(sourceLang = src, targetLang = tgt) != true) {
            _state.update { it.copy(message = str(R.string.err_mic)) }
            return
        }
        _state.update { it.copy(listening = side, speakingTurn = null) }
        watchWork()
    }

    /** Manual mode: talk button released. */
    fun stopTalking() {
        if (_state.value.listening == null) return
        // The indicator goes on first: stop() flushes the audio and, when the press held no
        // speech (a tap on the button), reports it through onNoSpeech at once — which used to
        // fire before the indicator was switched on, leaving "translating" on for good.
        _state.update { it.copy(listening = null, working = true, level = 0f) }
        session?.stop()
    }

    // Deliberately not persisted: the microphone should never open on launch by itself.
    fun setHandsFree(on: Boolean) {
        _state.update { it.copy(handsFree = on) }
        if (on) startHandsFree() else {
            session?.stop()
            busyWatcher?.cancel()
            _state.update { it.copy(listening = null, level = 0f, working = false) }
        }
    }

    private fun startHandsFree() {
        val s = _state.value
        if (s.phase != Phase.Ready) return
        // The microphone is muted while a translation is spoken (TranslatorSession.speak), so the
        // phone does not transcribe its own voice; both people then simply talk in turn.
        if (session?.startConversation(s.langA, s.langB) != true) {
            _state.update { it.copy(message = str(R.string.err_mic), handsFree = false) }
            return
        }
        _state.update { it.copy(listening = Side.A) }
        watchWork()
    }

    /** Reflects "the engine is transcribing/translating" without polling the UI thread hard. */
    private fun watchWork() {
        busyWatcher?.cancel()
        busyWatcher = viewModelScope.launch {
            while (isActive) {
                val pending = session?.translator?.pendingCount ?: 0
                if (pending > 0) _state.update { it.copy(working = true) }
                // Idle: the microphone is closed and nothing is being translated.
                val now = _state.value
                if (now.listening == null && !now.working) return@launch
                delay(200)
            }
        }
    }

    /** Stops the microphone without tearing the session down (screen paused). */
    fun pauseMic() {
        session?.stop()
        session?.stopSpeaking()
        busyWatcher?.cancel()
        _state.update { it.copy(listening = null, level = 0f, speakingTurn = null) }
    }

    fun resumeMic() {
        if (_state.value.handsFree && _state.value.phase == Phase.Ready) startHandsFree()
    }

    // ------------------------------------------------------------------ text

    fun translateText(text: String, from: Side, onDone: (Turn?) -> Unit) {
        val s = _state.value
        if (text.isBlank() || s.phase != Phase.Ready) { onDone(null); return }
        viewModelScope.launch {
            _state.update { it.copy(working = true) }
            val src = if (from == Side.A) s.langA else s.langB
            val tgt = if (from == Side.A) s.langB else s.langA
            val r = session?.translate(text, src, tgt)
            if (r == null || !r.ok) {
                _state.update { it.copy(working = false, message = r?.error ?: str(R.string.err_translate)) }
                onDone(null)
            } else {
                val turn = Turn(nextTurnId++, from, text, src, r.translatedText, tgt, r.route, r.totalMs)
                _state.update { it.copy(turns = it.turns + turn, working = false) }
                if (_state.value.speak) speakTurn(turn)
                onDone(turn)
            }
        }
    }

    // ---------------------------------------------------------------- camera

    /** Side whose language the last photo was read in, for reading it again after turning it. */
    private var cameraSide = Side.A

    /**
     * Reads the text in [image] (written in the language of [from]) and translates it into the
     * other language piece by piece, so each translation can be painted where its text stands. The
     * result joins the conversation like a typed sentence and is read aloud when speech is on.
     * With [findOrientation] a sideways or upside-down photo is turned upright first; the rotate
     * button passes false, since the user has chosen how the photo stands.
     */
    fun translateImage(image: Bitmap, from: Side, findOrientation: Boolean = true) {
        if (_state.value.phase != Phase.Ready) return
        noteLive()
        ocrJob?.cancel()
        ocrJob = viewModelScope.launch { readAndTranslate(image, from, findOrientation) }
    }

    /**
     * A photo picked from the gallery: opened and scaled off the main thread (a 50 MP photo, a
     * HEIC or a cloud-backed picture can take seconds), then read like a photo just taken. When
     * it cannot be opened the failure screen says why, so it can be reported.
     */
    fun openImage(uri: Uri, from: Side) {
        if (_state.value.phase != Phase.Ready) return
        noteLive()
        ocrJob?.cancel()
        ocrJob = viewModelScope.launch {
            _state.update { it.copy(camera = CameraPhase.Loading) }
            val decoded = runCatching { withContext(Dispatchers.IO) { PhotoFiles.load(getApplication<Application>(), uri) } }
            val bitmap = decoded.getOrNull()
            if (bitmap == null) {
                val why = decoded.exceptionOrNull()?.let { e -> "${e.javaClass.simpleName}: ${e.message ?: ""}" }.orEmpty()
                _state.update { it.copy(camera = CameraPhase.Failed(null, str(R.string.camera_image_failed_detail, why))) }
                return@launch
            }
            readAndTranslate(bitmap, from, findOrientation = true)
        }
    }

    private suspend fun readAndTranslate(image: Bitmap, from: Side, findOrientation: Boolean) {
        val s = _state.value
        val src = if (from == Side.A) s.langA else s.langB
        val tgt = if (from == Side.A) s.langB else s.langA
        cameraSide = from
        val model = s.ocrCatalog.firstOrNull { it.lang == src }
        if (model == null || src !in s.ocrInstalled) {
            _state.update { it.copy(camera = CameraPhase.Failed(image, str(R.string.ocr_not_installed, Lang.of(src).name))) }
            return
        }
        run {
            _state.update { it.copy(camera = CameraPhase.Working(image, translating = false)) }
            val ocrStart = SystemClock.elapsedRealtime()
            val read = runCatching {
                withContext(Dispatchers.Default) { OcrEngine.recognize(ocr.root, image, src, model.tessLang, findOrientation) }
            }
            val ocrMs = (SystemClock.elapsedRealtime() - ocrStart).toDouble()
            val page = read.getOrNull()
            if (page == null) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, read.exceptionOrNull()?.message ?: str(R.string.err_ocr))) }
                return@run
            }
            // The photo as it was read: turned upright when it was sideways.
            val image = page.image
            val regions = page.regions
            if (regions.isEmpty()) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, str(R.string.ocr_no_text))) }
                return@run
            }
            _state.update { it.copy(camera = CameraPhase.Working(image, translating = true)) }
            // Codes, dates, prices and phone numbers are not language and came back from Marian
            // as invented words: they stay as they are. The same text (a repeated label) is
            // translated once, and all the pieces go to the engine in one batch. A piece that
            // fails stays uncovered on the photo; only when every piece fails is it an error.
            val distinct = regions.map { it.text }.filter { OcrEngine.isTranslatable(it, src) }.distinct()
            val nmtStart = SystemClock.elapsedRealtime()
            val translations = HashMap<String, String>()
            var route: List<String> = emptyList()
            var error: String? = null
            if (distinct.isNotEmpty()) {
                val sources = distinct.map { OcrEngine.forTranslation(it, src) }
                // An engine without the batch call (an app updated ahead of its engine) throws
                // UnsatisfiedLinkError; the pieces then go one by one.
                val batch = runCatching { session?.translateLines(sources, src, tgt) }.getOrNull()
                val lines = batch?.takeIf { it.ok }?.lines?.takeIf { it.size == sources.size }
                if (lines != null) {
                    distinct.forEachIndexed { i, text -> translations[text] = lines[i].trim() }
                    route = batch.route
                } else {
                    if (batch != null && !batch.ok) error = batch.error
                    for ((i, text) in distinct.withIndex()) {
                        val r = session?.translate(sources[i], src, tgt)
                        if (r != null && r.ok) {
                            translations[text] = r.translatedText.trim()
                            if (route.isEmpty()) route = r.route
                        } else if (error == null) {
                            error = r?.error
                        }
                    }
                }
            }
            val nmtMs = (SystemClock.elapsedRealtime() - nmtStart).toDouble()
            val texts = regions.map { region ->
                PhotoText(region, translations[region.text].orEmpty(), kept = region.text !in distinct)
            }
            if (distinct.isNotEmpty() && texts.all { it.translation.isBlank() }) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, error ?: str(R.string.err_translate))) }
                return@run
            }
            val turn = Turn(
                nextTurnId++,
                from,
                texts.joinToString("\n") { it.region.text },
                src,
                texts.map { it.translation }.filter { it.isNotBlank() }.joinToString("\n"),
                tgt,
                route,
                ocrMs + nmtMs,
                fromImage = true,
            )
            _state.update { it.copy(turns = it.turns + turn, camera = CameraPhase.Result(image, turn, texts, ocrMs, nmtMs)) }
            if (_state.value.speak) speakTurn(turn)
        }
    }

    /**
     * Turns the photo on screen a quarter clockwise and reads it again as it now stands. A result
     * read the wrong way round leaves the conversation.
     */
    fun rotateCameraImage() {
        val phase = _state.value.camera
        val image = when (phase) {
            is CameraPhase.Result -> phase.image
            is CameraPhase.Failed -> phase.image
            else -> null
        } ?: return
        if (phase is CameraPhase.Result) _state.update { s -> s.copy(turns = s.turns.filterNot { it.id == phase.turn.id }) }
        translateImage(OcrEngine.prepare(image, 90), cameraSide, findOrientation = false)
    }

    /**
     * Back to the viewfinder after a photo; also cancels a recognition still running. A photo
     * taken out of live mode returns to live mode, whose recogniser is still open.
     */
    fun resetCamera() {
        ocrJob?.cancel()
        if (liveResume) startLive(liveSide) else _state.update { it.copy(camera = CameraPhase.Preview) }
    }

    /** Leaving the camera screen: live mode off and its recogniser closed. */
    fun leaveCamera() {
        ocrJob?.cancel()
        liveResume = false
        stopLive()
    }

    /** A photo taken while live mode was on comes back to it. */
    private fun noteLive() {
        if (_state.value.camera is CameraPhase.Live) liveResume = true
    }

    // ------------------------------------------------------------- live camera

    /**
     * Live mode: frame after frame of the viewfinder is read and its text painted over the scene,
     * as fast as the phone manages. Rough by design — a quarter of a photo's pixels, no second
     * reading to find which way round the frame stands, at most [LIVE_MAX_NEW] new pieces to the
     * engine per frame — so the shutter is still what gives the accurate reading. Text already
     * translated stays painted from [liveCache] while the phone moves, which is what makes the
     * following frames cheap.
     */
    fun startLive(from: Side) {
        if (_state.value.phase != Phase.Ready) return
        ocrJob?.cancel()
        liveResume = false
        liveSide = from
        synchronized(liveCache) { liveCache.clear() }
        _state.update { it.copy(camera = CameraPhase.Live()) }
    }

    /** Live mode off: back to the plain viewfinder, recogniser closed. */
    fun stopLive() {
        closeLive()
        _state.update { it.copy(camera = CameraPhase.Preview) }
    }

    /**
     * Whether the viewfinder should hand over the frame it has. False while a frame is still being
     * read or translated: the analyser then drops it rather than queueing work the phone cannot do.
     */
    fun wantsLiveFrame(): Boolean = !liveBusy && _state.value.camera is CameraPhase.Live

    /**
     * A viewfinder frame, already upright and scaled ([OcrEngine.LIVE_SIDE]); the view model owns
     * it from here and recycles it. Called from the analyser's thread.
     */
    fun liveFrame(frame: Bitmap, from: Side) {
        if (!wantsLiveFrame()) {
            frame.recycle()
            return
        }
        liveBusy = true
        // The other side's language means another recogniser and another language for the texts
        // already translated.
        if (from != liveSide) {
            liveSide = from
            synchronized(liveCache) { liveCache.clear() }
        }
        liveJob = viewModelScope.launch {
            try {
                readFrame(frame, from)
            } finally {
                liveBusy = false
            }
        }
    }

    private suspend fun readFrame(frame: Bitmap, from: Side) {
        val s = _state.value
        val src = if (from == Side.A) s.langA else s.langB
        val tgt = if (from == Side.A) s.langB else s.langA
        val model = s.ocrCatalog.firstOrNull { it.lang == src }
        if (s.camera !is CameraPhase.Live || model == null || src !in s.ocrInstalled) {
            frame.recycle()
            return
        }
        val width = frame.width
        val height = frame.height
        val ocrStart = SystemClock.elapsedRealtime()
        val page = withContext(liveDispatcher) {
            val reader = reader(src, model.tessLang)
            val read = if (reader == null) null else runCatching { reader.read(frame) }.getOrNull()
            frame.recycle()
            read
        }
        val ocrMs = (SystemClock.elapsedRealtime() - ocrStart).toDouble()
        // A frame the recogniser could not read leaves the last one painted rather than blinking.
        val regions = page?.regions ?: return
        val translatable = regions.map { it.text }.filter { OcrEngine.isTranslatable(it, src) }.toSet()

        fun paint(nmtMs: Double, pending: Int) {
            val texts = regions.map { r ->
                val kept = r.text !in translatable
                PhotoText(r, if (kept) "" else liveCached(r.text).orEmpty(), kept)
            }
            _state.update { st ->
                if (st.camera is CameraPhase.Live) {
                    st.copy(camera = CameraPhase.Live(texts, width, height, ocrMs, nmtMs, pending))
                } else {
                    st
                }
            }
        }

        // What this frame already knows is painted before the engine is asked anything, so a sign
        // read a moment ago stays on screen without waiting.
        val fresh = regions
            .filter { it.text in translatable && liveCached(it.text) == null }
            .distinctBy { it.text }
            // The largest text is the sign the phone is being pointed at; the small print can wait
            // for a frame that has room for it.
            .sortedByDescending { it.box.width().toLong() * it.box.height() }
            .take(LIVE_MAX_NEW)
            .map { it.text }
        paint(nmtMs = 0.0, pending = fresh.size)
        if (fresh.isEmpty()) return

        val nmtStart = SystemClock.elapsedRealtime()
        val sources = fresh.map { OcrEngine.forTranslation(it, src) }
        // As on the photo path, an engine without the batch call throws UnsatisfiedLinkError and
        // the pieces go one by one — which is why a frame hands over only a few of them.
        val batch = runCatching { session?.translateLines(sources, src, tgt) }.getOrNull()
        val lines = batch?.takeIf { it.ok }?.lines?.takeIf { it.size == sources.size }
        if (lines != null) {
            fresh.forEachIndexed { i, text -> liveRemember(text, lines[i].trim()) }
        } else {
            for ((i, text) in fresh.withIndex()) {
                if (!currentCoroutineContext().isActive) return
                val r = session?.translate(sources[i], src, tgt)
                if (r != null && r.ok) liveRemember(text, r.translatedText.trim())
            }
        }
        paint((SystemClock.elapsedRealtime() - nmtStart).toDouble(), pending = 0)
    }

    /** The open recogniser for [lang], opened or reopened when the side's language changed. Live thread only. */
    private fun reader(lang: String, tessLang: String): LiveOcr? {
        val open = liveReader
        if (open != null && liveLang == lang) return open
        open?.close()
        val fresh = runCatching { LiveOcr.open(ocr.root, lang, tessLang) }.getOrNull()
        liveReader = fresh
        liveLang = if (fresh != null) lang else null
        return fresh
    }

    private fun liveCached(text: String): String? = synchronized(liveCache) { liveCache[text] }

    private fun liveRemember(text: String, translation: String) {
        if (translation.isBlank()) return
        synchronized(liveCache) { liveCache[text] = translation }
    }

    /**
     * Closes the recogniser behind whatever frame is still being read: the close is queued on the
     * live thread, which is also the only thread that opens one, so the two can never cross.
     */
    private fun closeLive() {
        liveJob?.cancel()
        liveJob = null
        runCatching {
            liveExecutor.execute {
                liveReader?.close()
                liveReader = null
                liveLang = null
            }
        }
    }

    /** The text-recognition entry for [lang], null when the manifest does not offer one. */
    fun ocrModelFor(lang: String): OcrModel? = _state.value.ocrCatalog.firstOrNull { it.lang == lang }

    /** Fetches the text-recognition file for [lang] (a few MB) and reports progress in the state. */
    fun downloadOcr(lang: String) {
        val model = ocrModelFor(lang) ?: return
        if (_state.value.ocrDownload != null) return
        viewModelScope.launch {
            _state.update { it.copy(ocrDownload = OcrDownload(lang, 0, model.sizeBytes)) }
            val r = runCatching {
                ocr.install(model) { done, total -> _state.update { it.copy(ocrDownload = OcrDownload(lang, done, total)) } }
            }
            _state.update { it.copy(ocrDownload = null) }
            r.onFailure { e -> showMessage(e.message ?: str(R.string.err_download_failed)) }
            refreshOcr()
        }
    }

    fun removeOcr(lang: String) {
        val model = ocrModelFor(lang) ?: return
        viewModelScope.launch {
            withContext(Dispatchers.IO) { ocr.remove(model) }
            refreshOcr()
            _state.update { it.copy(message = str(R.string.msg_removed, str(R.string.ocr_model_title, Lang.of(lang).name))) }
        }
    }

    private fun refreshOcr() {
        viewModelScope.launch {
            val catalog = withContext(Dispatchers.IO) { ocr.catalog() }
            val installed = withContext(Dispatchers.IO) { ocr.installed(catalog) }.map { it.lang }.toSet()
            _state.update { it.copy(ocrCatalog = catalog, ocrInstalled = installed) }
        }
    }

    // -------------------------------------------------------------- settings

    fun setLanguages(a: String, b: String) {
        if (a == _state.value.langA && b == _state.value.langB) return
        prefs.langA = a
        prefs.langB = b
        pauseMic()
        _state.update { it.copy(langA = a, langB = b) }
        boot()
    }

    fun swapLanguages() = setLanguages(_state.value.langB, _state.value.langA)

    fun setSpeak(on: Boolean) {
        prefs.speak = on
        if (!on) stopSpeaking()
        _state.update { it.copy(speak = on) }
    }

    fun setSpeechRate(rate: Float) {
        prefs.speechRate = rate
        session?.speechRate = rate
        _state.update { it.copy(speechRate = rate) }
    }

    fun setBackend(backend: TranslationBackend) {
        if (backend == _state.value.backend) return
        prefs.backend = backend
        pauseMic()
        _state.update { it.copy(backend = backend) }
        boot()
    }

    /** Whisper on the GPU: reopens the engine with the new setting. */
    fun setUseGpu(on: Boolean) {
        if (on == _state.value.useGpu) return
        prefs.useGpu = on
        pauseMic()
        _state.update { it.copy(useGpu = on) }
        boot()
    }

    /**
     * Switches the speech model: [id] null leaves the choice to [ModelPolicy]. One that is not
     * installed yet goes through the normal download screen.
     */
    fun setStt(id: String?) {
        val s = _state.value
        val auto = id == null
        val effective = id ?: ModelPolicy.sttFor(s.langs, prefs.roomy)
        prefs.sttAuto = auto
        if (!auto) prefs.sttId = id
        if (auto == s.sttAuto && effective == s.sttId) return
        pauseMic()
        _state.update { it.copy(sttAuto = auto, sttId = effective) }
        boot()
    }

    /** Switches the LLM, as [setStt] does the speech model. */
    fun setLlm(id: String?) {
        val s = _state.value
        val auto = id == null
        val effective = id ?: ModelPolicy.llmFor(s.langs, prefs.roomy)
        prefs.llmAuto = auto
        if (!auto) prefs.llmId = id
        if (auto == s.llmAuto && effective == s.llmId) return
        pauseMic()
        _state.update { it.copy(llmAuto = auto, llmId = effective) }
        boot()
    }

    /** The language that makes the automatic choice pick whisper medium for the current pair, null when it picks small. */
    fun autoSttReason(): String? = ModelPolicy.sttReason(_state.value.langs, prefs.roomy)

    fun clearConversation() {
        stopSpeaking()
        _state.update { it.copy(turns = emptyList()) }
    }

    fun showMessage(text: String) = _state.update { it.copy(message = text) }

    fun dismissMessage() = _state.update { it.copy(message = null) }

    fun refreshInstalled() {
        viewModelScope.launch {
            val list = withContext(Dispatchers.IO) { runCatching { repo?.status() }.getOrNull() } ?: return@launch
            _state.update {
                it.copy(installed = list, llmOptions = list.filter { m -> m.kind == "llm" }, sttOptions = list.filter { m -> m.kind == "stt" })
            }
        }
        refreshOcr()
    }

    fun removeModel(id: String) {
        val name = _state.value.installed.firstOrNull { it.id == id }
            ?.let { modelTitle(getApplication<Application>(), it) } ?: id
        viewModelScope.launch {
            withContext(Dispatchers.IO) { repo?.remove(id) }
            refreshInstalled()
            _state.update { it.copy(message = str(R.string.msg_removed, name)) }
        }
    }

    /** Engine build info for the settings screen. */
    fun engineInfo(): List<Pair<String, String>> {
        val caps = runCatching { OfflineTranslator.buildCapabilities }.getOrNull()
        val pipeline = runCatching { session?.translator?.capabilities }.getOrNull()
        val unknown = str(R.string.value_unknown)
        fun flag(key: String) = when (caps?.optBoolean(key)) {
            true -> str(R.string.value_included)
            false -> str(R.string.value_not_included)
            null -> unknown
        }
        return listOf(
            str(R.string.info_engine_version) to (runCatching { OfflineTranslator.version }.getOrNull() ?: unknown),
            str(R.string.info_manifest) to (repo?.manifestVersion?.ifEmpty { unknown } ?: unknown),
            str(R.string.info_whisper) to flag("whisper"),
            str(R.string.info_ct2) to flag("ctranslate2"),
            str(R.string.info_llama) to flag("llama"),
            str(R.string.info_rnnoise) to flag("rnnoise"),
            str(R.string.info_llm_loaded) to when (pipeline?.optBoolean("llm_loaded")) {
                true -> str(R.string.value_yes)
                false -> str(R.string.value_no)
                null -> unknown
            },
        )
    }

    override fun onCleared() {
        busyWatcher?.cancel()
        val s = session
        val r = repo
        session = null
        repo = null
        // close() joins native threads — never on the main thread.
        Thread { runCatching { s?.close() }; runCatching { r?.close() } }.start()
        closeLive()
        liveExecutor.shutdown()
    }

    private fun str(id: Int, vararg args: Any): String =
        getApplication<Application>().getString(id, *args)

    private fun label(m: ModelStatus): String = modelTitle(getApplication<Application>(), m)

    private fun labelOf(list: List<ModelStatus>, id: String): String =
        list.firstOrNull { it.id == id }?.let { label(it) } ?: id
}

fun mb(bytes: Long): String = when {
    bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1_000_000_000.0)
    bytes >= 1_000_000 -> "${bytes / 1_000_000} MB"
    bytes >= 1_000 -> "${bytes / 1_000} KB"
    else -> "$bytes B"
}
