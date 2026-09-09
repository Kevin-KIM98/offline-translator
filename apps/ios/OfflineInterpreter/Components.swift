import SwiftUI

/// Press-and-hold microphone button. Holding is deliberate: it makes the utterance boundary
/// explicit, which is both faster and more reliable than waiting for silence detection.
struct TalkButton: View {
    let label: String
    let accent: Color
    let active: Bool
    let level: Float
    let enabled: Bool
    let onPress: () -> Void
    let onRelease: () -> Void

    @Environment(\.colorScheme) private var scheme
    @State private var pressing = false

    var body: some View {
        VStack(spacing: 6) {
            Image(systemName: "mic.fill").font(.system(size: 24, weight: .medium))
            Text(label).font(.headline).lineLimit(1).minimumScaleFactor(0.6)
            Text(active ? L("talk_listening") : L("talk_hold"))
                .font(.caption2)
                .opacity(0.75)
                .lineLimit(1)
        }
        .foregroundColor(enabled ? (active ? .white : accent) : Palette.muted(scheme))
        .frame(maxWidth: .infinity)
        .frame(height: 104)
        .background(
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .fill(enabled ? (active ? accent : accent.opacity(0.16)) : Palette.muted(scheme).opacity(0.12))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .strokeBorder(
                    active ? Color.white.opacity(0.25 + 0.45 * Double(level)) : accent.opacity(0.35),
                    lineWidth: active ? 1 + 3 * CGFloat(level) : 1
                )
        )
        .animation(.easeOut(duration: 0.12), value: active)
        .animation(.easeOut(duration: 0.09), value: level)
        .contentShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard enabled, !pressing else { return }
                    pressing = true
                    onPress()
                }
                .onEnded { _ in
                    guard pressing else { return }
                    pressing = false
                    onRelease()
                }
        )
        .disabled(!enabled)
    }
}

/// Level meter drawn as a row of bars — cheap, and readable in bright light.
struct LevelBars: View {
    let level: Float
    let accent: Color
    var bars = 12

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<bars, id: \.self) { i in
                let threshold = Float(i + 1) / Float(bars)
                let on = level >= threshold
                RoundedRectangle(cornerRadius: 2)
                    .fill(on ? accent : accent.opacity(0.2))
                    .frame(width: 4, height: on ? CGFloat(8 + 14 * threshold) : 6)
            }
        }
        .animation(.easeOut(duration: 0.08), value: level)
    }
}

struct SectionLabel: View {
    let text: String
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        Text(text.uppercased())
            .font(.caption)
            .kerning(0.4)
            .foregroundColor(Palette.muted(scheme))
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.leading, 4)
            .padding(.bottom, 6)
    }
}

/// Bottom sheet listing the seven languages.
struct LanguagePicker: View {
    let title: String
    let current: String
    let disabled: String?
    let onPick: (String) -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationView {
            List(Lang.all) { lang in
                Button {
                    onPick(lang.code)
                    dismiss()
                } label: {
                    HStack {
                        Text(lang.name)
                        Spacer()
                        if lang.code == current {
                            Image(systemName: "checkmark").foregroundColor(.accentColor)
                        }
                    }
                }
                .disabled(lang.code == disabled)
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(L("picker_close")) { dismiss() }
                }
            }
        }
    }
}
