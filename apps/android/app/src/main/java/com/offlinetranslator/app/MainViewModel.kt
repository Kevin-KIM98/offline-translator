package com.offlinetranslator.app

import android.app.Application
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

    /** Models are missing; [pending] is what the chosen languages still need. */
    data class Setup(val pending: List<ModelStatus>, val error: String? = null) : Phase

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
)

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
            handsFree = prefs.handsFree,
            speechRate = prefs.speechRate,
            backend = prefs.backend,
        )
    )
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var repo: ModelRepository? = null
    private var session: TranslatorSession? = null
    private var downloadJob: Job? = null
    private var busyWatcher: Job? = null
    private var nextTurnId = 1L

    /** Side the user is currently holding the talk button for, in manual mode. */
    private var pendingSide: Side = Side.A

    init {
        boot()
    }

    // ---------------------------------------------------------------- startup

    fun boot() {
        viewModelScope.launch {
            _state.update { it.copy(phase = Phase.Checking) }
            val r = runCatching {
                withContext(Dispatchers.IO) {
                    val repository = repo ?: ModelRepository(getApplication<Application>()).also { repo = it }
                    repository.refreshManifest()
                    if (!repository.hasManifest()) error("모델 목록을 받지 못했습니다. 네트워크를 확인해 주세요.")
                    repository.statusForLanguages(_state.value.langs, llmMode = llmMode())
                }
            }
            r.onFailure { e -> _state.update { it.copy(phase = Phase.Fatal(e.message ?: "초기화 실패")) } }
            r.onSuccess { status ->
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
                    it.copy(phase = Phase.Setup(pending, "저장 공간이 부족합니다. ${mb(total)} 이상 필요합니다."))
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
            }.onFailure { e -> failure = e.message ?: "다운로드 실패" }

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
                runCatching { repo?.statusForLanguages(_state.value.langs, llmMode = llmMode())?.filter { it.needsDownload } }
                    .getOrNull()
            } ?: emptyList()
            _state.update {
                it.copy(phase = Phase.Setup(pending, "다운로드를 멈췄습니다. 다시 받으면 이어서 내려받습니다."))
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
                    val config = repository.pipelineConfig(backend = _state.value.backend)
                    // The app drives TTS itself so a line can be replayed and muted mid-sentence.
                    TranslatorSession(getApplication<Application>(), config, speakResults = false)
                }
            }
            built.onFailure { e -> _state.update { it.copy(phase = Phase.Fatal(e.message ?: "엔진을 열지 못했습니다")) } }
            built.onSuccess { s ->
                s.speechRate = _state.value.speechRate
                s.onResult = { r -> onEngineResult(r) }
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
            _state.update { it.copy(message = r.error ?: "번역 실패", working = false) }
            return
        }
        if (r.sourceText.isBlank()) {
            _state.update { it.copy(working = false) }
            return
        }
        // Hands-free listens with source "auto" toward B. When the engine reports that B's
        // language was spoken, the other party answered — translate that back into A instead.
        val answeredBack = s.handsFree && s.langA != s.langB && r.sourceLang == s.langB
        if (answeredBack) {
            viewModelScope.launch {
                val back = session?.translate(r.sourceText, r.sourceLang, s.langA)
                if (back == null || !back.ok) {
                    _state.update { it.copy(message = back?.error ?: "번역 실패", working = false) }
                } else {
                    addTurn(back, Side.B)
                }
            }
            return
        }
        val side = if (s.handsFree) (if (r.sourceLang == s.langB) Side.B else Side.A) else pendingSide
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
        session?.start(sourceLang = src, targetLang = tgt)
        _state.update { it.copy(listening = side, speakingTurn = null) }
        watchWork()
    }

    /** Manual mode: talk button released. */
    fun stopTalking() {
        if (_state.value.listening == null) return
        session?.stop()
        _state.update { it.copy(listening = null, working = true, level = 0f) }
    }

    fun setHandsFree(on: Boolean) {
        prefs.handsFree = on
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
        session?.start(sourceLang = "auto", targetLang = s.langB)
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
                val now = _state.value
                if (pending == 0 && now.listening == null && !now.working) return@launch
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
                _state.update { it.copy(working = false, message = r?.error ?: "번역 실패") }
                onDone(null)
            } else {
                val turn = Turn(nextTurnId++, from, text, src, r.translatedText, tgt, r.route, r.totalMs)
                _state.update { it.copy(turns = it.turns + turn, working = false) }
                if (_state.value.speak) speakTurn(turn)
                onDone(turn)
            }
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

    fun clearConversation() {
        stopSpeaking()
        _state.update { it.copy(turns = emptyList()) }
    }

    fun dismissMessage() = _state.update { it.copy(message = null) }

    fun refreshInstalled() {
        viewModelScope.launch {
            val list = withContext(Dispatchers.IO) { runCatching { repo?.status() }.getOrNull() } ?: return@launch
            _state.update { it.copy(installed = list) }
        }
    }

    fun removeModel(id: String) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) { repo?.remove(id) }
            refreshInstalled()
            _state.update { it.copy(message = "삭제했습니다: $id") }
        }
    }

    /** Engine build info for the settings screen. */
    fun engineInfo(): List<Pair<String, String>> {
        val caps = runCatching { OfflineTranslator.buildCapabilities }.getOrNull()
        val pipeline = runCatching { session?.translator?.capabilities }.getOrNull()
        fun flag(key: String) = when (caps?.optBoolean(key)) {
            true -> "사용"
            false -> "미사용"
            null -> "-"
        }
        return listOf(
            "엔진 버전" to (runCatching { OfflineTranslator.version }.getOrNull() ?: "-"),
            "모델 목록" to (repo?.manifestVersion ?: "-"),
            "음성인식 whisper" to flag("whisper"),
            "번역 CTranslate2" to flag("ctranslate2"),
            "LLM llama.cpp" to flag("llama"),
            "잡음 제거 RNNoise" to flag("rnnoise"),
            "LLM 로드됨" to when (pipeline?.optBoolean("llm_loaded")) {
                true -> "예"; false -> "아니오"; null -> "-"
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

    private fun label(m: ModelStatus): String = when (m.kind) {
        "stt" -> "음성인식 모델"
        "llm" -> "다국어 LLM"
        "nmt" -> "번역 모델 ${m.pair}"
        else -> m.id
    }

    private fun labelOf(list: List<ModelStatus>, id: String): String =
        list.firstOrNull { it.id == id }?.let { label(it) } ?: id
}

fun mb(bytes: Long): String = when {
    bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1_000_000_000.0)
    bytes >= 1_000_000 -> "${bytes / 1_000_000} MB"
    bytes >= 1_000 -> "${bytes / 1_000} KB"
    else -> "$bytes B"
}
