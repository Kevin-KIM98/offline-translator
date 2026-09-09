import SwiftUI
import UIKit

struct ConversationView: View {
    let onSettings: () -> Void

    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var scheme
    @State private var picking: Side?
    @State private var typing = false

    var body: some View {
        VStack(spacing: 0) {
            languageBar
            transcript
            controls
        }
        .sheet(item: $picking) { side in
            LanguagePicker(
                title: side == .a ? L("picker_left") : L("picker_right"),
                current: side == .a ? model.langA : model.langB,
                disabled: side == .a ? model.langB : model.langA
            ) { code in
                if side == .a {
                    model.setLanguages(a: code, b: model.langB)
                } else {
                    model.setLanguages(a: model.langA, b: code)
                }
            }
        }
        .sheet(isPresented: $typing) {
            TextInputSheet().environmentObject(model)
        }
        .alert(L("error_title"), isPresented: Binding(
            get: { model.message != nil },
            set: { if !$0 { model.message = nil } }
        )) {
            Button(L("ok")) { model.message = nil }
        } message: {
            Text(model.message ?? "")
        }
    }

    // MARK: - Pieces

    private var languageBar: some View {
        HStack(spacing: 8) {
            chip(Lang.of(model.langA).name, Palette.sideA(scheme)) { picking = .a }
            Button { model.swapLanguages() } label: {
                Image(systemName: "arrow.left.arrow.right").foregroundColor(Palette.muted(scheme))
            }
            chip(Lang.of(model.langB).name, Palette.sideB(scheme)) { picking = .b }
            Button(action: onSettings) {
                Image(systemName: "gearshape").foregroundColor(Palette.muted(scheme))
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Palette.surface(scheme).ignoresSafeArea(edges: .top))
    }

    private func chip(_ name: String, _ accent: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(name)
                .font(.subheadline.weight(.semibold))
                .foregroundColor(accent)
                .lineLimit(1)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .background(RoundedRectangle(cornerRadius: 12).fill(accent.opacity(0.14)))
        }
    }

    @ViewBuilder
    private var transcript: some View {
        if model.turns.isEmpty {
            VStack(spacing: 10) {
                Spacer()
                Text("\(Lang.of(model.langA).name) ↔ \(Lang.of(model.langB).name)")
                    .font(.title3.weight(.semibold))
                Text(L("empty_hint"))
                    .font(.subheadline)
                    .foregroundColor(Palette.muted(scheme))
                    .multilineTextAlignment(.center)
                Text(L("empty_offline"))
                    .font(.caption)
                    .foregroundColor(Palette.sideA(scheme))
                    .padding(.top, 8)
                Spacer()
            }
            .padding(32)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 12) {
                        ForEach(model.turns) { turn in
                            TurnCard(turn: turn, speaking: model.speakingTurn == turn.id) {
                                if model.speakingTurn == turn.id { model.stopSpeaking() } else { model.speakTurn(turn) }
                            }
                            .id(turn.id)
                            .contextMenu {
                                Button {
                                    UIPasteboard.general.string = turn.translatedText
                                } label: {
                                    Label(L("copy_translation"), systemImage: "doc.on.doc")
                                }
                                Button {
                                    UIPasteboard.general.string = turn.sourceText
                                } label: {
                                    Label(L("copy_source"), systemImage: "text.quote")
                                }
                            }
                        }
                    }
                    .padding(16)
                }
                .onChange(of: model.turns.count) { _ in
                    if let last = model.turns.last {
                        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
        }
    }

    private var controls: some View {
        VStack(spacing: 10) {
            if model.working {
                HStack(spacing: 8) {
                    ProgressView().scaleEffect(0.7)
                    Text(L("translating")).font(.caption).foregroundColor(Palette.sideA(scheme))
                    Spacer()
                }
            }

            HStack {
                Button {
                    model.setHandsFree(!model.handsFree)
                } label: {
                    Label(L("hands_free"), systemImage: "waveform")
                        .font(.subheadline)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 7)
                        .background(
                            Capsule().fill(model.handsFree
                                ? Palette.sideA(scheme).opacity(0.2)
                                : Palette.muted(scheme).opacity(0.12))
                        )
                        .foregroundColor(model.handsFree ? Palette.sideA(scheme) : Palette.muted(scheme))
                }
                Spacer()
                Button { typing = true } label: {
                    Image(systemName: "keyboard").foregroundColor(Palette.muted(scheme))
                }
                Button { model.clearConversation() } label: {
                    Image(systemName: "trash").foregroundColor(Palette.muted(scheme))
                }
                .disabled(model.turns.isEmpty)
            }

            if !model.micGranted {
                micCard
            } else if model.handsFree {
                handsFreePanel
            } else {
                HStack(spacing: 12) {
                    TalkButton(
                        label: Lang.of(model.langA).name,
                        accent: Palette.sideA(scheme),
                        active: model.listening == .a,
                        level: model.level,
                        enabled: model.listening != .b,
                        onPress: { model.startTalking(.a) },
                        onRelease: { model.stopTalking() }
                    )
                    TalkButton(
                        label: Lang.of(model.langB).name,
                        accent: Palette.sideB(scheme),
                        active: model.listening == .b,
                        level: model.level,
                        enabled: model.listening != .a,
                        onPress: { model.startTalking(.b) },
                        onRelease: { model.stopTalking() }
                    )
                }
            }
        }
        .padding(16)
        .background(Palette.surface(scheme).ignoresSafeArea(edges: .bottom))
    }

    private var micCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(L("mic_needed_title"), systemImage: "mic.slash")
                .font(.subheadline.weight(.semibold))
            Text(L("mic_needed_body"))
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
            HStack {
                Button(L("mic_allow")) { Task { await model.requestPermission() } }
                    .buttonStyle(.borderedProminent)
                Button(L("mic_open_settings")) {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                }
            }
            .padding(.top, 4)
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 18).fill(Palette.muted(scheme).opacity(0.12)))
    }

    private var handsFreePanel: some View {
        HStack {
            VStack(alignment: .leading, spacing: 6) {
                Text(L("hands_free_title"))
                    .font(.subheadline.weight(.semibold))
                    .foregroundColor(Palette.sideA(scheme))
                Text(L("hands_free_body"))
                    .font(.caption)
                    .foregroundColor(Palette.muted(scheme))
                LevelBars(level: model.level, accent: Palette.sideA(scheme)).padding(.top, 4)
            }
            Spacer()
            Button { model.setHandsFree(false) } label: {
                Image(systemName: "stop.circle.fill")
                    .font(.system(size: 34))
                    .foregroundColor(Palette.sideA(scheme))
            }
        }
        .padding(18)
        .background(RoundedRectangle(cornerRadius: 22).fill(Palette.sideA(scheme).opacity(0.12)))
    }
}

extension Side: Identifiable {
    var id: Int { self == .a ? 0 : 1 }
}

private struct TurnCard: View {
    let turn: Turn
    let speaking: Bool
    let onReplay: () -> Void

    @Environment(\.colorScheme) private var scheme

    private var accent: Color { turn.side == .a ? Palette.sideA(scheme) : Palette.sideB(scheme) }

    var body: some View {
        HStack(spacing: 0) {
            if turn.side == .b { Spacer(minLength: 28) }
            HStack(spacing: 0) {
                Rectangle().fill(accent).frame(width: 4)
                VStack(alignment: .leading, spacing: 8) {
                    Text(turn.sourceText)
                        .font(.footnote)
                        .foregroundColor(Palette.muted(scheme))
                    Text(turn.translatedText.isEmpty ? L("no_translation") : turn.translatedText)
                        .font(.title3.weight(.semibold))
                    HStack {
                        Text(footer)
                            .font(.caption2)
                            .foregroundColor(Palette.muted(scheme))
                        Spacer()
                        Button(action: onReplay) {
                            Image(systemName: speaking ? "stop.circle" : "speaker.wave.2")
                                .foregroundColor(speaking ? accent : Palette.muted(scheme))
                        }
                    }
                }
                .padding(14)
            }
            .fixedSize(horizontal: false, vertical: true)
            .cardBackground(scheme, radius: 18)
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            if turn.side == .a { Spacer(minLength: 28) }
        }
    }

    private var footer: String {
        let route = turn.route.isEmpty ? "" : " · " + turn.route.joined(separator: "→")
        return L("turn_footer", Lang.of(turn.sourceLang).name, Lang.of(turn.targetLang).name, Int(turn.totalMs)) + route
    }
}

private struct TextInputSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @Environment(\.colorScheme) private var scheme
    @State private var text = ""
    @State private var side: Side = .a

    var body: some View {
        NavigationView {
            VStack(alignment: .leading, spacing: 12) {
                Picker(L("settings_languages"), selection: $side) {
                    Text(Lang.of(model.langA).name).tag(Side.a)
                    Text(Lang.of(model.langB).name).tag(Side.b)
                }
                .pickerStyle(.segmented)

                TextEditor(text: $text)
                    .frame(minHeight: 120)
                    .padding(6)
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(Palette.muted(scheme).opacity(0.4)))

                Button {
                    model.translateText(text, from: side)
                    dismiss()
                } label: {
                    Text(L("text_translate")).frame(maxWidth: .infinity).padding(.vertical, 6)
                }
                .buttonStyle(.borderedProminent)
                .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

                Spacer()
            }
            .padding(20)
            .navigationTitle(L("text_sheet_title"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button(L("picker_close")) { dismiss() } }
            }
        }
    }
}
