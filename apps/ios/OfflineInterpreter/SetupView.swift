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
            Text(L("setup_title")).font(.title3.weight(.semibold))
            Text(L("setup_subtitle", Lang.of(model.langA).name, Lang.of(model.langB).name))
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
                        Text(L("setup_llm_note"))
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

            Label(L("setup_wifi_hint"), systemImage: "wifi")
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .padding(.bottom, 12)

            Button {
                model.download()
            } label: {
                Label(L("setup_download", formatBytes(total)), systemImage: "arrow.down.circle")
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)

            Button(L("setup_change_languages"), action: onSettings)
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
            Text(verifying ? L("verify_title") : L("download_title")).font(.title3.weight(.semibold))
            Text(label)
                .font(.subheadline)
                .foregroundColor(Palette.muted(scheme))
                .padding(.top, 6)

            ProgressView(value: fraction)
                .padding(.top, 20)

            Text(L("download_progress", formatBytes(done), formatBytes(total), Int(fraction * 100)))
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .padding(.top, 10)

            Text(L("download_keep_open"))
                .font(.caption)
                .foregroundColor(Palette.muted(scheme))
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(.top, 28)

            Button(L("download_stop")) { model.cancelDownload() }
                .frame(maxWidth: .infinity)
                .padding(.top, 12)
        }
        .padding(32)
    }
}
