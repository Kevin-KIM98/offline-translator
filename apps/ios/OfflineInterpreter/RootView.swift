import SwiftUI

struct RootView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var scheme
    @State private var showSettings = false

    var body: some View {
        ZStack {
            Palette.background(scheme).ignoresSafeArea()
            content
        }
        // Settings stays reachable during setup so the languages can be changed before a
        // 0.5 GB download starts.
        .sheet(isPresented: $showSettings) {
            SettingsView()
                .environmentObject(model)
        }
    }

    @ViewBuilder
    private var content: some View {
        switch model.phase {
        case .checking:
            Busy(message: "모델을 확인하는 중")
        case .opening:
            Busy(message: "통역 엔진을 준비하는 중")
        case let .setup(pending, error):
            SetupView(pending: pending, error: error, onSettings: { showSettings = true })
        case let .downloading(label, done, total, verifying):
            DownloadView(label: label, done: done, total: total, verifying: verifying)
        case let .fatal(message):
            Failure(message: message) { Task { await model.boot() } }
        case .ready:
            ConversationView(onSettings: { showSettings = true })
        }
    }
}

private struct Busy: View {
    let message: String
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        VStack(spacing: 18) {
            ProgressView().scaleEffect(1.3)
            Text(message).foregroundColor(Palette.muted(scheme))
        }
    }
}

private struct Failure: View {
    let message: String
    let onRetry: () -> Void
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        VStack(spacing: 12) {
            Text("시작할 수 없습니다").font(.headline)
            Text(message)
                .font(.subheadline)
                .foregroundColor(Palette.muted(scheme))
                .multilineTextAlignment(.center)
            Button(action: onRetry) {
                Text("다시 시도").frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .padding(.top, 12)
        }
        .padding(32)
    }
}
