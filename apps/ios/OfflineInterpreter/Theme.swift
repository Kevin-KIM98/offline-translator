import SwiftUI

/// The two speakers get a warm/cool pair so a glance tells you whose line you are reading.
/// Everything else stays near-neutral: the translated sentence is the only loud thing on screen.
enum Palette {
    static func sideA(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.17, green: 0.68, blue: 0.62) : Color(red: 0.05, green: 0.43, blue: 0.39)
    }

    static func sideB(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.88, green: 0.64, blue: 0.24) : Color(red: 0.54, green: 0.35, blue: 0.04)
    }

    static func background(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.055, green: 0.082, blue: 0.075) : Color(red: 0.965, green: 0.969, blue: 0.961)
    }

    static func surface(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.086, green: 0.118, blue: 0.110) : .white
    }

    static func muted(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.62, green: 0.69, blue: 0.67) : Color(red: 0.33, green: 0.39, blue: 0.37)
    }
}

extension View {
    /// Card surface used throughout the app.
    func cardBackground(_ scheme: ColorScheme, radius: CGFloat = 16) -> some View {
        background(
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(Palette.surface(scheme))
        )
    }
}

func formatBytes(_ bytes: UInt64) -> String {
    if bytes >= 1_000_000_000 { return String(format: "%.1f GB", Double(bytes) / 1_000_000_000) }
    if bytes >= 1_000_000 { return "\(bytes / 1_000_000) MB" }
    if bytes >= 1_000 { return "\(bytes / 1_000) KB" }
    return "\(bytes) B"
}
