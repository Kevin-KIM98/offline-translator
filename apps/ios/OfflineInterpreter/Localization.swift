import Foundation

/// Localized text lookup. Keys live in en.lproj / ko.lproj Localizable.strings and match the
/// Android string resource names one for one.
///
/// SwiftUI's `Text("literal")` would localize on its own, but only for literals; going through
/// these helpers keeps formatted strings and non-Text call sites on the same path.
func L(_ key: String) -> String {
    NSLocalizedString(key, comment: "")
}

func L(_ key: String, _ args: CVarArg...) -> String {
    String(format: NSLocalizedString(key, comment: ""), locale: .current, arguments: args)
}
