import AVFoundation
import Foundation
import OfflineTranslator
import SwiftUI

/// One of the seven languages the engine supports, as shown in the UI.
struct Lang: Identifiable, Hashable {
    let code: String
    /// Endonym — what speakers of the language call it.
    let name: String

    var id: String { code }

    static let all = [
        Lang(code: "ko", name: "한국어"),
        Lang(code: "en", name: "English"),
        Lang(code: "es", name: "Español"),
        Lang(code: "ja", name: "日本語"),
        Lang(code: "zh", name: "中文"),
        Lang(code: "vi", name: "Tiếng Việt"),
        Lang(code: "th", name: "ไทย"),
    ]

    static func of(_ code: String) -> Lang {
        all.first { $0.code == code } ?? Lang(code: code, name: code.uppercased())
    }
}

enum Side: Hashable {
    case a, b
    var other: Side { self == .a ? .b : .a }
}

/// One line of the conversation.
struct Turn: Identifiable, Hashable {
    let id: Int
    let side: Side
    let sourceText: String
    let sourceLang: String
    let translatedText: String
    let targetLang: String
    let route: [String]
    let totalMs: Double
}

/// Where the app is in its lifecycle: models first, then a live session.
enum Phase: Equatable {
    case checking
    case setup(pending: [OTModelStatus], error: String?)
    case downloading(label: String, done: UInt64, total: UInt64, verifying: Bool)
    case opening
    case ready
    case fatal(String)

    static func == (lhs: Phase, rhs: Phase) -> Bool {
        switch (lhs, rhs) {
        case (.checking, .checking), (.opening, .opening), (.ready, .ready): return true
        case let (.setup(lp, le), .setup(rp, re)):
            return lp.map(\.identifier) == rp.map(\.identifier) && le == re
        case let (.downloading(ll, ld, lt, lv), .downloading(rl, rd, rt, rv)):
            return ll == rl && ld == rd && lt == rt && lv == rv
        case let (.fatal(l), .fatal(r)): return l == r
        default: return false
        }
    }
}

@MainActor
final class AppModel: ObservableObject {

    @Published private(set) var phase: Phase = .checking
    @Published private(set) var turns: [Turn] = []
    @Published private(set) var listening: Side?
    @Published private(set) var working = false
    @Published private(set) var speakingTurn: Int?
    @Published private(set) var level: Float = 0
    @Published private(set) var installed: [OTModelStatus] = []
    @Published var message: String?
    @Published private(set) var micGranted = false

    @Published var langA: String { didSet { defaults.set(langA, forKey: "langA") } }
    @Published var langB: String { didSet { defaults.set(langB, forKey: "langB") } }
    @Published var speak: Bool {
        didSet {
            defaults.set(speak, forKey: "speak")
            if !speak { stopSpeaking() }
        }
    }
    @Published var handsFree = false
    @Published var speechRate: Double {
        didSet {
            defaults.set(speechRate, forKey: "speechRate")
            session?.speechRate = AVSpeechUtteranceDefaultSpeechRate * Float(speechRate)
        }
    }
    @Published var backend: OTTranslationBackend {
        didSet {
            defaults.set(backend.rawValue, forKey: "backend")
            if backend != oldValue { Task { await boot() } }
        }
    }

    private let defaults = UserDefaults.standard
    private var downloader: ModelDownloader?
    private var session: TranslatorSession?
    private var downloadTask: Task<Void, Never>?
    private var workWatcher: Task<Void, Never>?
    private var nextTurnId = 1
    private var pendingSide: Side = .a

    init() {
        langA = defaults.string(forKey: "langA") ?? "ko"
        langB = defaults.string(forKey: "langB") ?? "en"
        speak = defaults.object(forKey: "speak") as? Bool ?? true
        speechRate = defaults.object(forKey: "speechRate") as? Double ?? 1.0
        backend = OTTranslationBackend(rawValue: defaults.integer(forKey: "backend")) ?? .auto
    }

    var languages: [String] { [langA, langB] }

    private var llmMode: OTLlmMode { backend == .LLM ? .always : .ifNeeded }

    // MARK: - Startup

    func boot() async {
        phase = .checking
        let downloader = self.downloader ?? ModelDownloader()
        self.downloader = downloader

        _ = await downloader.refreshManifest()
        guard downloader.hasManifest else {
            phase = .fatal("모델 목록을 받지 못했습니다. 네트워크를 확인해 주세요.")
            return
        }
        let pending = downloader.status(languages: languages, llmMode: llmMode).filter(\.needsDownload)
        if pending.isEmpty {
            await openSession()
        } else {
            phase = .setup(pending: pending, error: nil)
        }
    }

    func refreshPermission() {
        if #available(iOS 17.0, *) {
            micGranted = AVAudioApplication.shared.recordPermission == .granted
        } else {
            micGranted = AVAudioSession.sharedInstance().recordPermission == .granted
        }
    }

    func requestPermission() async {
        micGranted = await AudioCapture.requestPermission()
    }

    func download() {
        guard case let .setup(pending, _) = phase, let downloader else { return }
        let total = pending.reduce(UInt64(0)) { $0 + $1.totalBytes }
        guard downloader.freeBytes() > total + 50_000_000 else {
            phase = .setup(pending: pending, error: "저장 공간이 부족합니다. \(formatBytes(total)) 이상 필요합니다.")
            return
        }
        downloadTask?.cancel()
        phase = .downloading(label: Self.label(pending[0]), done: 0, total: total, verifying: false)

        downloadTask = Task { [weak self] in
            var completed: UInt64 = 0
            var failure: String?
            for await event in downloader.installAll(pending) {
                guard let self else { return }
                switch event {
                case let .started(id, _):
                    self.phase = .downloading(label: Self.label(pending, id), done: completed, total: total, verifying: false)
                case let .downloading(id, _, done, _):
                    self.phase = .downloading(label: Self.label(pending, id), done: completed + done, total: total, verifying: false)
                case let .verifying(id, _, _):
                    self.phase = .downloading(label: Self.label(pending, id), done: completed, total: total, verifying: true)
                case let .installed(id):
                    completed += pending.first { $0.identifier == id }?.totalBytes ?? 0
                case let .failed(id, reason):
                    failure = "\(Self.label(pending, id)): \(reason)"
                case .allDone:
                    break
                }
            }
            guard let self, !Task.isCancelled else { return }
            if let failure {
                self.phase = .setup(pending: pending, error: failure)
            } else {
                await self.openSession()
            }
        }
    }

    func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        let pending = downloader?.status(languages: languages, llmMode: llmMode).filter(\.needsDownload) ?? []
        phase = .setup(pending: pending, error: "다운로드를 멈췄습니다. 다시 받으면 이어서 내려받습니다.")
    }

    private func openSession() async {
        phase = .opening
        session?.shutdown()
        session = nil
        guard let downloader else {
            phase = .fatal("모델 관리자가 없습니다")
            return
        }
        do {
            let created = try TranslatorSession(config: downloader.manager.pipelineConfig(with: backend))
            // The app drives TTS itself so a line can be replayed and muted mid-sentence.
            created.speakResults = false
            created.speechRate = AVSpeechUtteranceDefaultSpeechRate * Float(speechRate)
            created.onResult = { [weak self] r in self?.handle(r) }
            created.onError = { [weak self] msg in
                self?.message = msg
                self?.working = false
            }
            created.onAudioLevel = { [weak self] value in self?.level = value }
            session = created
            phase = .ready
            refreshInstalled()
            if handsFree { startHandsFree() }
        } catch {
            phase = .fatal(error.localizedDescription)
        }
    }

    // MARK: - Conversation

    private func handle(_ r: OTTranslationResult) {
        guard r.ok else {
            message = r.error ?? "번역 실패"
            working = false
            return
        }
        guard !r.sourceText.isEmpty else {
            working = false
            return
        }
        // Hands-free listens with source "auto" toward B. When the engine reports that B's
        // language was spoken, the other party answered — translate that back into A instead.
        if handsFree, langA != langB, r.sourceLang == langB {
            Task { [weak self] in
                guard let self, let session = self.session else { return }
                let back = await session.translate(r.sourceText, from: r.sourceLang, to: self.langA)
                if back.ok {
                    self.append(back, side: .b)
                } else {
                    self.message = back.error ?? "번역 실패"
                    self.working = false
                }
            }
            return
        }
        let side: Side = handsFree ? (r.sourceLang == langB ? .b : .a) : pendingSide
        append(r, side: side)
    }

    private func append(_ r: OTTranslationResult, side: Side) {
        let turn = Turn(
            id: nextTurnId,
            side: side,
            sourceText: r.sourceText,
            sourceLang: r.sourceLang,
            translatedText: r.translatedText,
            targetLang: r.targetLang,
            route: r.route,
            totalMs: r.totalMs
        )
        nextTurnId += 1
        turns.append(turn)
        working = false
        if speak { speakTurn(turn) }
    }

    func speakTurn(_ turn: Turn) {
        Task { [weak self] in
            guard let self, let session = self.session else { return }
            self.speakingTurn = turn.id
            _ = await session.speak(turn.translatedText, lang: turn.targetLang)
            if self.speakingTurn == turn.id { self.speakingTurn = nil }
        }
    }

    func stopSpeaking() {
        session?.stopSpeaking()
        speakingTurn = nil
    }

    func startTalking(_ side: Side) {
        guard phase == .ready, !handsFree, let session else { return }
        pendingSide = side
        session.stopSpeaking()
        let src = side == .a ? langA : langB
        let tgt = side == .a ? langB : langA
        do {
            try session.start(sourceLang: src, targetLang: tgt)
            listening = side
            speakingTurn = nil
            watchWork()
        } catch {
            message = error.localizedDescription
        }
    }

    func stopTalking() {
        guard listening != nil else { return }
        session?.stop()
        listening = nil
        working = true
        level = 0
    }

    func setHandsFree(_ on: Bool) {
        handsFree = on
        if on {
            startHandsFree()
        } else {
            session?.stop()
            workWatcher?.cancel()
            listening = nil
            level = 0
            working = false
        }
    }

    private func startHandsFree() {
        guard phase == .ready, let session else { return }
        do {
            try session.start(sourceLang: "auto", targetLang: langB)
            listening = .a
            watchWork()
        } catch {
            message = error.localizedDescription
            handsFree = false
        }
    }

    /// Reflects "the engine is transcribing/translating" without a callback for it.
    private func watchWork() {
        workWatcher?.cancel()
        workWatcher = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, let session = self.session else { return }
                let pending = session.pipeline.pendingUtteranceCount
                if pending > 0 { self.working = true }
                if pending == 0, self.listening == nil, !self.working { return }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
        }
    }

    func pauseMic() {
        session?.stop()
        session?.stopSpeaking()
        workWatcher?.cancel()
        listening = nil
        level = 0
        speakingTurn = nil
    }

    func resumeMic() {
        if handsFree, phase == .ready { startHandsFree() }
    }

    // MARK: - Text

    func translateText(_ text: String, from side: Side) {
        guard phase == .ready, let session, !text.isEmpty else { return }
        Task { [weak self] in
            guard let self else { return }
            self.working = true
            let src = side == .a ? self.langA : self.langB
            let tgt = side == .a ? self.langB : self.langA
            let r = await session.translate(text, from: src, to: tgt)
            guard r.ok else {
                self.working = false
                self.message = r.error ?? "번역 실패"
                return
            }
            let turn = Turn(id: self.nextTurnId, side: side, sourceText: text, sourceLang: src,
                            translatedText: r.translatedText, targetLang: tgt, route: r.route, totalMs: r.totalMs)
            self.nextTurnId += 1
            self.turns.append(turn)
            self.working = false
            if self.speak { self.speakTurn(turn) }
        }
    }

    // MARK: - Settings

    func setLanguages(a: String, b: String) {
        guard a != langA || b != langB else { return }
        pauseMic()
        langA = a
        langB = b
        Task { await boot() }
    }

    func swapLanguages() { setLanguages(a: langB, b: langA) }

    func clearConversation() {
        stopSpeaking()
        turns.removeAll()
    }

    func refreshInstalled() {
        installed = downloader?.status() ?? []
    }

    func remove(_ status: OTModelStatus) {
        _ = downloader?.manager.removeModel(status.identifier)
        refreshInstalled()
        message = "삭제했습니다: \(Self.label(status))"
    }

    struct InfoRow: Identifiable {
        let key: String
        let value: String
        var id: String { key }
    }

    var engineInfo: [InfoRow] {
        let caps = OTTranslationPipeline.buildCapabilities()
        func flag(_ key: String) -> String {
            guard let value = caps[key] as? Bool else { return "-" }
            return value ? "사용" : "미사용"
        }
        let loaded = (session?.pipeline.capabilities["llm_loaded"] as? Bool).map { $0 ? "예" : "아니오" } ?? "-"
        return [
            InfoRow(key: "엔진 버전", value: OTTranslationPipeline.version()),
            InfoRow(key: "모델 목록", value: downloader?.manager.manifestVersion ?? "-"),
            InfoRow(key: "음성인식 whisper", value: flag("whisper")),
            InfoRow(key: "번역 CTranslate2", value: flag("ctranslate2")),
            InfoRow(key: "LLM llama.cpp", value: flag("llama")),
            InfoRow(key: "잡음 제거 RNNoise", value: flag("rnnoise")),
            InfoRow(key: "LLM 로드됨", value: loaded),
        ]
    }

    // MARK: - Labels

    static func label(_ m: OTModelStatus) -> String {
        switch m.kind {
        case "stt": return "음성 인식"
        case "llm": return "다국어 통역 LLM"
        case "nmt":
            let parts = m.pair.split(separator: "-")
            if parts.count == 2 {
                return "\(Lang.of(String(parts[0])).name) → \(Lang.of(String(parts[1])).name) 번역"
            }
            return "번역 모델 \(m.pair)"
        default: return m.identifier
        }
    }

    static func subtitle(_ m: OTModelStatus) -> String {
        switch m.kind {
        case "stt": return "whisper · 7개 언어 공용"
        case "llm": return "Qwen2.5 · 모든 언어 조합"
        case "nmt": return "OPUS-MT"
        default: return m.kind
        }
    }

    private static func label(_ list: [OTModelStatus], _ id: String) -> String {
        list.first { $0.identifier == id }.map(label) ?? id
    }
}
