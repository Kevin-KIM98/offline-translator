import OfflineTranslator
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @Environment(\.colorScheme) private var scheme
    @State private var picking: Side?

    var body: some View {
        NavigationView {
            Form {
                Section(L("settings_languages")) {
                    Button { picking = .a } label: {
                        row(L("settings_speaker_left"), Lang.of(model.langA).name, Palette.sideA(scheme))
                    }
                    Button { picking = .b } label: {
                        row(L("settings_speaker_right"), Lang.of(model.langB).name, Palette.sideB(scheme))
                    }
                }

                Section(L("settings_sound")) {
                    Toggle(L("settings_speak"), isOn: $model.speak)
                    VStack(alignment: .leading) {
                        HStack {
                            Text(L("settings_rate"))
                            Spacer()
                            Text(L("settings_rate_value", model.speechRate))
                                .foregroundColor(Palette.muted(scheme))
                        }
                        Slider(value: $model.speechRate, in: 0.6...1.6, step: 0.2)
                    }
                }

                Section {
                    Picker(L("settings_backend"), selection: $model.backend) {
                        Text(L("backend_auto")).tag(OTTranslationBackend.auto)
                        Text(L("backend_marian")).tag(OTTranslationBackend.marian)
                        Text(L("backend_llm")).tag(OTTranslationBackend.LLM)
                    }
                    .pickerStyle(.segmented)
                    Text(backendExplanation)
                        .font(.caption)
                        .foregroundColor(Palette.muted(scheme))
                } header: {
                    Text(L("settings_backend"))
                }

                Section(L("settings_installed")) {
                    let ready = model.installed.filter { $0.state == .ready }
                    if ready.isEmpty {
                        Text(L("settings_none_installed")).foregroundColor(Palette.muted(scheme))
                    } else {
                        ForEach(ready, id: \.identifier) { status in
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(AppModel.label(status))
                                    Text(formatBytes(status.totalBytes))
                                        .font(.caption)
                                        .foregroundColor(Palette.muted(scheme))
                                }
                                Spacer()
                                Button {
                                    model.remove(status)
                                } label: {
                                    Image(systemName: "trash").foregroundColor(Palette.muted(scheme))
                                }
                                .accessibilityLabel(L("delete"))
                                .buttonStyle(.borderless)
                            }
                        }
                        HStack {
                            Text(L("settings_total")).foregroundColor(Palette.muted(scheme))
                            Spacer()
                            Text(formatBytes(ready.reduce(0) { $0 + $1.totalBytes }))
                                .foregroundColor(Palette.muted(scheme))
                        }
                    }
                }

                Section(L("settings_about")) {
                    ForEach(model.engineInfo) { row in
                        HStack {
                            Text(row.key)
                            Spacer()
                            Text(row.value).foregroundColor(Palette.muted(scheme))
                        }
                    }
                }

                Section {
                    Text(L("settings_privacy"))
                        .font(.caption)
                        .foregroundColor(Palette.muted(scheme))
                }
            }
            .navigationTitle(L("settings"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button(L("settings_done")) { dismiss() } }
            }
            .onAppear { model.refreshInstalled() }
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
        }
    }

    private func row(_ title: String, _ value: String, _ accent: Color) -> some View {
        HStack {
            Text(title).foregroundColor(.primary)
            Spacer()
            Text(value).foregroundColor(accent)
        }
    }

    private var backendExplanation: String {
        switch model.backend {
        case .marian:
            return L("backend_marian_desc")
        case .LLM:
            return L("backend_llm_desc")
        default:
            return L("backend_auto_desc")
        }
    }
}
