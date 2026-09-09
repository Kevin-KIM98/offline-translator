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
                Section("언어") {
                    Button { picking = .a } label: {
                        row("왼쪽 화자", Lang.of(model.langA).name, Palette.sideA(scheme))
                    }
                    Button { picking = .b } label: {
                        row("오른쪽 화자", Lang.of(model.langB).name, Palette.sideB(scheme))
                    }
                }

                Section("소리") {
                    Toggle("번역 읽어주기", isOn: $model.speak)
                    VStack(alignment: .leading) {
                        HStack {
                            Text("말하기 속도")
                            Spacer()
                            Text(String(format: "%.1f×", model.speechRate))
                                .foregroundColor(Palette.muted(scheme))
                        }
                        Slider(value: $model.speechRate, in: 0.6...1.6, step: 0.2)
                    }
                }

                Section {
                    Picker("번역 엔진", selection: $model.backend) {
                        Text("자동").tag(OTTranslationBackend.auto)
                        Text("전용 모델").tag(OTTranslationBackend.marian)
                        Text("LLM").tag(OTTranslationBackend.LLM)
                    }
                    .pickerStyle(.segmented)
                    Text(backendExplanation)
                        .font(.caption)
                        .foregroundColor(Palette.muted(scheme))
                } header: {
                    Text("번역 엔진")
                }

                Section("설치된 모델") {
                    let ready = model.installed.filter { $0.state == .ready }
                    if ready.isEmpty {
                        Text("아직 설치된 모델이 없습니다.").foregroundColor(Palette.muted(scheme))
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
                                .buttonStyle(.borderless)
                            }
                        }
                        HStack {
                            Text("합계").foregroundColor(Palette.muted(scheme))
                            Spacer()
                            Text(formatBytes(ready.reduce(0) { $0 + $1.totalBytes }))
                                .foregroundColor(Palette.muted(scheme))
                        }
                    }
                }

                Section("정보") {
                    ForEach(model.engineInfo) { row in
                        HStack {
                            Text(row.key)
                            Spacer()
                            Text(row.value).foregroundColor(Palette.muted(scheme))
                        }
                    }
                }

                Section {
                    Text("모든 처리는 기기 안에서 이루어집니다. 음성과 문장은 어디로도 전송되지 않습니다.")
                        .font(.caption)
                        .foregroundColor(Palette.muted(scheme))
                }
            }
            .navigationTitle("설정")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("완료") { dismiss() } }
            }
            .onAppear { model.refreshInstalled() }
            .sheet(item: $picking) { side in
                LanguagePicker(
                    title: side == .a ? "왼쪽 화자의 언어" : "오른쪽 화자의 언어",
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
            return "전용 번역 모델만 사용합니다. 태국어처럼 모델이 없는 방향은 번역되지 않습니다."
        case .LLM:
            return "모든 방향을 LLM 하나로 번역합니다. 느리지만 언어를 자동으로 알아냅니다. 1.1 GB가 필요합니다."
        default:
            return "언어쌍 전용 모델이 있으면 그것을 쓰고, 없으면 LLM으로 번역합니다. 가장 빠릅니다."
        }
    }
}
