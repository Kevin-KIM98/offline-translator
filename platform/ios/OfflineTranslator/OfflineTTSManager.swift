#if canImport(OfflineTranslatorObjC)
import OfflineTranslatorObjC
#endif
import AVFoundation

/// OS text-to-speech (AVSpeechSynthesizer). Works offline whenever the language's voice is
/// installed (Settings → Accessibility → Spoken Content → Voices).
public final class OfflineTTSManager: NSObject, AVSpeechSynthesizerDelegate {

    private let synthesizer = AVSpeechSynthesizer()
    private var completions: [ObjectIdentifier: (Bool) -> Void] = [:]

    public override init() {
        super.init()
        synthesizer.delegate = self
    }

    public static func voice(for lang: String) -> AVSpeechSynthesisVoice? {
        let code: String
        switch lang {
        case "ko": code = "ko-KR"
        case "en": code = "en-US"
        case "ja": code = "ja-JP"
        case "zh": code = "zh-CN"
        case "es": code = "es-ES"
        case "vi": code = "vi-VN"
        case "th": code = "th-TH"
        default: code = lang
        }
        // Prefer enhanced/premium voices when downloaded; they sound far better than compact.
        let candidates = AVSpeechSynthesisVoice.speechVoices().filter { $0.language == code }
        if #available(iOS 16.0, macOS 13.0, *),
           let premium = candidates.first(where: { $0.quality == .premium }) {
            return premium
        }
        return candidates.first { $0.quality == .enhanced }
            ?? AVSpeechSynthesisVoice(language: code)
    }

    public static func isLanguageAvailable(_ lang: String) -> Bool { voice(for: lang) != nil }

    /// Fire-and-forget. Returns false if no voice is available for `lang`.
    @discardableResult
    public func speak(_ text: String, lang: String, rate: Float = AVSpeechUtteranceDefaultSpeechRate, interrupt: Bool = true) -> Bool {
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              let voice = Self.voice(for: lang) else { return false }
        if interrupt, synthesizer.isSpeaking { synthesizer.stopSpeaking(at: .immediate) }
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = voice
        utterance.rate = rate
        synthesizer.speak(utterance)
        return true
    }

    /// Suspends until playback ends (or fails). Returns false if the language is unavailable.
    public func speakAndWait(_ text: String, lang: String, rate: Float = AVSpeechUtteranceDefaultSpeechRate) async -> Bool {
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              let voice = Self.voice(for: lang) else { return false }
        if synthesizer.isSpeaking { synthesizer.stopSpeaking(at: .immediate) }
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = voice
        utterance.rate = rate
        return await withCheckedContinuation { cont in
            completions[ObjectIdentifier(utterance)] = { cont.resume(returning: $0) }
            synthesizer.speak(utterance)
        }
    }

    public func stop() {
        synthesizer.stopSpeaking(at: .immediate)
    }

    public var isSpeaking: Bool { synthesizer.isSpeaking }

    // MARK: AVSpeechSynthesizerDelegate

    public func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        completions.removeValue(forKey: ObjectIdentifier(utterance))?(true)
    }

    public func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        completions.removeValue(forKey: ObjectIdentifier(utterance))?(false)
    }
}
