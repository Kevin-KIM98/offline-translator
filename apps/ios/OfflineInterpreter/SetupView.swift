import OfflineTranslator
import SwiftUI

struct SetupView: View {
    let pending: [OTModelStatus]
    let error: String?
    let onSettings: () -> Void

    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var scheme

    private var total: UInt64 { pending.reduce(0) { $0 + $1.totalBytes } }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("통역을 시작하기 전에").font(.title3.weight(.semibold))
            Text("\(Lang.of(model.langA).name)와 \(Lang.of(model.langB).name)를 통역하려면 아래 모델을 한 번만 내려받으면 됩니다. 그 뒤로는 인터넷 없이 동작합니다.")
                .font(.subheadline)
                .foregroundColor(Palette.muted(scheme))
                .padding(.top, 8)

            ScrollView {
                VStack(spacing: 8) {
                    ForEach(pending, id: \.identifier) { status in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(AppModel.label(status))
                                Text(AppModel.subtitle(status))
                                    .font(.caption)
                                    .foregroundColor(Palette.muted(scheme))
                            }
                            Spacer()
                            Text(formatBytes(status.totalBytes))
                                .font(.subheadline.weight(.medium))
                                .foregroundColor(Palette.muted(scheme))
                        }
                        .padding(16)
                        .cardBackground(scheme)
                    }

                    if pending.contains(where: { $0.kind == "llm" }) {
                        Text("선택한 언어 조합에는 직접 번역 모델이 없어서 다국어 LLM이 함께 설치됩니다. 설정에서 다른 언어를 고르면 용량을 줄일 수 있습니다.")
                            .font(.caption)
                            .foregroundColor(Palette.muted(scheme))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(14)
                            .background(RoundedRectangle(cornerRadius: 14).fill(Palette.muted(scheme).opacity(0.12)))
                    }
                }
                .padding(.vertical, 20)
            }

            if let error {
                Text(error)
                    .font(.caption)
                    .foregroundColor(.red)
                    .padding(14)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 14).fill(Color.red.opacity(0.12)))
                    .padding(.bottom, 12)
            }

            Label("Wi-Fi에서 받는 것을 권합니다. 중단해도 이어받습니다.", systemImage: "wifi")
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .padding(.bottom, 12)

            Button {
                model.download()
            } label: {
                Label("\(formatBytes(total)) 내려받기", systemImage: "arrow.down.circle")
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)

            Button("언어 바꾸기", action: onSettings)
                .frame(maxWidth: .infinity)
                .padding(.top, 6)
        }
        .padding(24)
    }
}

struct DownloadView: View {
    let label: String
    let done: UInt64
    let total: UInt64
    let verifying: Bool

    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var scheme

    private var fraction: Double { total > 0 ? min(1, Double(done) / Double(total)) : 0 }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(verifying ? "파일을 검사하는 중" : "모델을 내려받는 중").font(.title3.weight(.semibold))
            Text(label)
                .font(.subheadline)
                .foregroundColor(Palette.muted(scheme))
                .padding(.top, 6)

            ProgressView(value: fraction)
                .padding(.top, 20)

            Text("\(formatBytes(done)) / \(formatBytes(total))  ·  \(Int(fraction * 100))%")
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .padding(.top, 10)

            Text("이 화면을 켜 둔 채로 기다려 주세요.\n앱을 닫으면 받은 지점부터 다시 이어집니다.")
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(.top, 28)

            Button("멈추기") { model.cancelDownload() }
                .frame(maxWidth: .infinity)
                .padding(.top, 12)
        }
        .padding(32)
    }
}
