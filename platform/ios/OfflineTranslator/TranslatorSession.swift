import Foundation

/// Live conversation glue: mic → engine (denoise + VAD + STT + NMT) → AVSpeechSynthesizer.
///
///     let session = try TranslatorSession(config: downloader.manager.pipelineConfig())
///     session.onResult = { r in print(r.sourceText, "→", r.translatedText) }
///     try session.start(sourceLang: "auto", targetLang: "en")
@MainActor
final class TranslatorSession {

    let pipeline: OTTranslationPipeline
    let tts = OfflineTTSManager()
    private let capture = AudioCapture()
    private var worker: Task<Void, Never>?
    private let wakeups = AsyncStream<Void>.makeStream()

    var sourceLang = "auto"
    var targetLang = "en"
    var speakResults = true

    var onResult: ((OTTranslationResult) -> Void)?
    var onError: ((String) -> Void)?
    var onSpeechState: ((Bool) -> Void)?

    var isRunning: Bool { capture.isRunning }

    init(config: OTPipelineConfig) throws {
        pipeline = try OTTranslationPipeline(config: config)
        capture.onFrames = { [pipeline, wakeups] ptr, count in
            // Audio thread: denoise + segmentation only.
            if pipeline.feedAudio(ptr, count: UInt(count)) { wakeups.continuation.yield() }
        }
        capture.onError = { [weak self] msg in Task { @MainActor in self?.onError?(msg) } }
    }

    func start(sourceLang: String? = nil, targetLang: String? = nil) throws {
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
    func endUtterance() {
        if pipeline.flushAudio() { wakeups.continuation.yield() }
    }

    func stop() {
        capture.stop()
        endUtterance()
    }

    /// Text-only path (keyboard input). Runs on a background thread.
    func translate(_ text: String, from src: String, to tgt: String) async -> OTTranslationResult {
        let pipeline = self.pipeline
        return await Task.detached { pipeline.translateText(text, sourceLang: src, targetLang: tgt) }.value
    }

    func shutdown() {
        stop()
        worker?.cancel()
        wakeups.continuation.finish()
    }
}
