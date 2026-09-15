package com.offlinetranslator.app

import android.app.Application
import android.graphics.Bitmap
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
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Where the app is in its lifecycle: models first, then a live session. */
sealed interface Phase {
    /** Reading the manifest and checking what is installed. */
    data object Checking : Phase

    /**
     * Models are missing; [pending] is what the chosen languages still need — or, with
     * [everything], every model the manifest offers, so no later choice needs a download.
     */
    data class Setup(val pending: List<ModelStatus>, val error: String? = null, val everything: Boolean = false) : Phase

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

/** A piece of text read from a photo and its translation (blank when that piece failed). */
data class PhotoText(val region: OcrRegion, val translation: String)

/** What the camera screen shows. */
sealed interface CameraPhase {
    data object Preview : CameraPhase

    /** Reading the photo ([translating] false) or translating what was read (true), [done] of [total] pieces. */
    data class Working(val image: Bitmap, val translating: Boolean, val done: Int = 0, val total: Int = 0) : CameraPhase

    /** Every piece of text in [image] with its translation, to be painted where the text stands. */
    data class Result(val image: Bitmap, val turn: Turn, val texts: List<PhotoText>) : CameraPhase

    data class Failed(val image: Bitmap?, val message: String) : CameraPhase
}

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
    /** Chosen LLM id, null for the manifest's default; every LLM the manifest offers. */
    val llmId: String? = null,
    val llmOptions: List<ModelStatus> = emptyList(),
    /** Chosen speech model id, null for the manifest's default; every whisper model the manifest offers. */
    val sttId: String? = null,
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
            llmId = prefs.llmId,
            sttId = prefs.sttId,
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
        viewModelScope.launch {
            _state.update { it.copy(phase = Phase.Checking) }
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
        val pending = (_state.value.phase as? Phase.Setup)?.pending ?: return
        val total = pending.sumOf { it.totalBytes }
        downloadJob?.cancel()
        downloadJob = viewModelScope.launch {
            val repository = repo ?: return@launch
            val free = withContext(Dispatchers.IO) { repository.freeBytes() }
            if (free < total + 50L * 1024 * 1024) {
                _state.update {
                    it.copy(phase = Phase.Setup(pending, str(R.string.err_storage, mb(total))))
                }
                return@launch
            }
            _state.update { it.copy(phase = Phase.Downloading(label(pending.first()), 0, total)) }
            var completed = 0L
            var failure: String? = null
            runCatching {
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
                        is InstallEvent.Failed -> failure = "${labelOf(pending, ev.id)}: ${ev.reason}"
                        is InstallEvent.AllDone -> {}
                    }
                }
            }.onFailure { e -> failure = e.message ?: str(R.string.err_download_failed) }

            val err = failure
            if (err != null) {
                _state.update { it.copy(phase = Phase.Setup(pending, err)) }
            } else {
                prefs.setupDone = true
                openSession()
            }
        }
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
     * in one download, so changing languages or models later never waits for the network. The
     * setup screen lists them with the total size and asks before starting.
     */
    fun downloadEverything() {
        viewModelScope.launch {
            val pending = withContext(Dispatchers.IO) { runCatching { repo?.status() }.getOrNull() }
                ?.filter { it.needsDownload } ?: emptyList()
            if (pending.isEmpty()) {
                showMessage(str(R.string.all_models_installed))
                return@launch
            }
            pauseMic()
            _state.update { it.copy(phase = Phase.Setup(pending, everything = true), handsFree = false, listening = null) }
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
        session?.stop()
        _state.update { it.copy(listening = null, working = true, level = 0f) }
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

    /**
     * Reads the text in [image] (written in the language of [from]) and translates it into the
     * other language piece by piece, so each translation can be painted where its text stands. The
     * result joins the conversation like a typed sentence and is read aloud when speech is on.
     */
    fun translateImage(image: Bitmap, from: Side) {
        val s = _state.value
        if (s.phase != Phase.Ready) return
        val src = if (from == Side.A) s.langA else s.langB
        val tgt = if (from == Side.A) s.langB else s.langA
        val model = s.ocrCatalog.firstOrNull { it.lang == src }
        if (model == null || src !in s.ocrInstalled) {
            _state.update { it.copy(camera = CameraPhase.Failed(image, str(R.string.ocr_not_installed, Lang.of(src).name))) }
            return
        }
        ocrJob?.cancel()
        ocrJob = viewModelScope.launch {
            _state.update { it.copy(camera = CameraPhase.Working(image, translating = false)) }
            val read = runCatching {
                withContext(Dispatchers.Default) { OcrEngine.recognize(ocr.root, image, src, model.tessLang) }
            }
            val regions = read.getOrNull()
            if (regions == null) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, read.exceptionOrNull()?.message ?: str(R.string.err_ocr))) }
                return@launch
            }
            if (regions.isEmpty()) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, str(R.string.ocr_no_text))) }
                return@launch
            }
            _state.update { it.copy(camera = CameraPhase.Working(image, translating = true, done = 0, total = regions.size)) }
            // The same text (a repeated label, a price heading) is translated once. A piece that
            // fails stays uncovered on the photo; only when every piece fails is it an error.
            val translations = HashMap<String, String>()
            val texts = ArrayList<PhotoText>()
            var route: List<String> = emptyList()
            var ms = 0.0
            var firstError: String? = null
            for ((i, region) in regions.withIndex()) {
                val translated = translations.getOrPut(region.text) {
                    val r = session?.translate(OcrEngine.forTranslation(region.text, src), src, tgt)
                    if (r != null && r.ok) {
                        if (route.isEmpty()) route = r.route
                        ms += r.totalMs
                        r.translatedText.trim()
                    } else {
                        if (firstError == null) firstError = r?.error
                        ""
                    }
                }
                texts += PhotoText(region, translated)
                _state.update { it.copy(camera = CameraPhase.Working(image, translating = true, done = i + 1, total = regions.size)) }
            }
            if (texts.all { it.translation.isBlank() }) {
                _state.update { it.copy(camera = CameraPhase.Failed(image, firstError ?: str(R.string.err_translate))) }
                return@launch
            }
            val turn = Turn(
                nextTurnId++,
                from,
                texts.joinToString("\n") { it.region.text },
                src,
                texts.map { it.translation }.filter { it.isNotBlank() }.joinToString("\n"),
                tgt,
                route,
                ms,
                fromImage = true,
            )
            _state.update { it.copy(turns = it.turns + turn, camera = CameraPhase.Result(image, turn, texts)) }
            if (_state.value.speak) speakTurn(turn)
        }
    }

    /** Back to the viewfinder; also cancels a recognition still running. */
    fun resetCamera() {
        ocrJob?.cancel()
        _state.update { it.copy(camera = CameraPhase.Preview) }
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

    /** Switches the speech model. One that is not installed yet goes through the normal download screen. */
    fun setStt(id: String?) {
        if (id == _state.value.sttId) return
        prefs.sttId = id
        pauseMic()
        _state.update { it.copy(sttId = id) }
        boot()
    }

    /** Switches the LLM. One that is not installed yet goes through the normal download screen. */
    fun setLlm(id: String?) {
        if (id == _state.value.llmId) return
        prefs.llmId = id
        pauseMic()
        _state.update { it.copy(llmId = id) }
        boot()
    }

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
