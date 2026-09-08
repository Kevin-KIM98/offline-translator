#if canImport(OfflineTranslatorObjC)
import OfflineTranslatorObjC
#endif
import Foundation

/// Live conversation glue: mic → engine (denoise + VAD + STT + NMT) → AVSpeechSynthesizer.
///
///     let session = try TranslatorSession(config: downloader.manager.pipelineConfig())
///     session.onResult = { r in print(r.sourceText, "→", r.translatedText) }
///     try session.start(sourceLang: "auto", targetLang: "en")
@MainActor
public final class TranslatorSession {

    public let pipeline: OTTranslationPipeline
    public let tts = OfflineTTSManager()
    private let capture = AudioCapture()
    private var worker: Task<Void, Never>?
    private let wakeups = AsyncStream<Void>.makeStream()

    public var sourceLang = "auto"
    public var targetLang = "en"
    public var speakResults = true

    public var onResult: ((OTTranslationResult) -> Void)?
    public var onError: ((String) -> Void)?
    public var onSpeechState: ((Bool) -> Void)?

    public var isRunning: Bool { capture.isRunning }

    public init(config: OTPipelineConfig) throws {
        pipeline = try OTTranslationPipeline(config: config)
        capture.onFrames = { [pipeline, wakeups] ptr, count in
            // Audio thread: denoise + segmentation only.
            if pipeline.feedAudio(ptr, count: UInt(count)) { wakeups.continuation.yield() }
        }
        capture.onError = { [weak self] msg in Task { @MainActor in self?.onError?(msg) } }
    }

    public func start(sourceLang: String? = nil, targetLang: String? = nil) throws {
        if let s = sourceLang { self.sourceLang = s }
        if let t = targetLang { self.targetLang = t }
        guard !capture.isRunning else { return }
        pipeline.resetAudio()

        worker = Task.detached(priority: .userInitiated) { [weak self, pipeline, wakeups] in
            for await _ in wakeups.stream {
                while pipeline.pendingUtteranceCount > 0, !Task.isCancelled {
                    guard let self = self else { return }
                    let (src, tgt, speak) = await MainActor.run { (self.sourceLang, self.targetLang, self.speakResults) }
                    await MainActor.run { self.tts.stop() }
                    let r = pipeline.processPending(withSourceLang: src, targetLang: tgt)
                    await MainActor.run {
                        if !r.ok { self.onError?(r.error ?? "unknown error") }
                        if !r.isEmpty { self.onResult?(r) }
                    }
                    if r.ok, speak, !r.isEmpty, !r.translatedText.isEmpty {
                        await MainActor.run { self.onSpeechState?(true) }
                        _ = await self.tts.speakAndWait(r.translatedText, lang: r.targetLang)
                        await MainActor.run { self.onSpeechState?(false) }
                    }
                }
            }
        }
        try capture.start()
    }

    /// Push-to-talk release: close the current utterance immediately.
    public func endUtterance() {
        if pipeline.flushAudio() { wakeups.continuation.yield() }
    }

    public func stop() {
        capture.stop()
        endUtterance()
    }

    /// Text-only path (keyboard input). Runs on a background thread.
    public func translate(_ text: String, from src: String, to tgt: String) async -> OTTranslationResult {
        let pipeline = self.pipeline
        return await Task.detached { pipeline.translateText(text, sourceLang: src, targetLang: tgt) }.value
    }

    public func shutdown() {
        stop()
        worker?.cancel()
        wakeups.continuation.finish()
    }

    /// One-call setup: fetch the manifest, download what `languages` need (with progress) and
    /// return a ready session. Safe on every launch — installed models are skipped.
    ///
    ///     let session = try await TranslatorSession.prepare(languages: ["ko", "en"]) { done, total in ... }
    ///     session.onResult = { r in ... }
    ///     try session.start(sourceLang: "ko", targetLang: "en")
    public static func prepare(languages: [String],
                        manifestURL: URL? = ModelDownloader.defaultManifestURL,
                        backend: OTTranslationBackend = .auto,
                        llmMode: OTLlmMode = .ifNeeded,
                        progress: ((UInt64, UInt64) -> Void)? = nil) async throws -> TranslatorSession {
        let downloader = ModelDownloader(manifestURL: manifestURL)
        _ = await downloader.refreshManifest()
        guard downloader.hasManifest else {
            throw ModelDownloader.InstallError(message: "no model manifest available (offline on first launch?)")
        }
        let effectiveLlm: OTLlmMode = backend == .LLM ? .always : llmMode
        let needed = downloader.status(languages: languages, llmMode: effectiveLlm).filter { $0.needsDownload }
        let total = needed.reduce(UInt64(0)) { $0 + $1.totalBytes }
        if !needed.isEmpty {
            if downloader.freeBytes() < total + 50_000_000 {
                throw ModelDownloader.InstallError(message: "not enough storage: need \(total / 1_000_000) MB")
            }
            var completed: UInt64 = 0
            var failure: String?
            for await event in downloader.installAll(needed) {
                switch event {
                case .downloading(_, _, let done, _): progress?(completed + done, total)
                case .installed(let id):
                    completed += needed.first { $0.identifier == id }?.totalBytes ?? 0
                    progress?(completed, total)
                case .failed(let id, let reason): failure = "\(id): \(reason)"
                default: break
                }
            }
            if let f = failure { throw ModelDownloader.InstallError(message: "model download failed: \(f)") }
        }
        return try TranslatorSession(config: downloader.manager.pipelineConfig(with: backend))
    }
}
