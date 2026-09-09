import SwiftUI
import UIKit

@main
struct OfflineInterpreterApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .task {
                    // A conversation is held with the screen facing the other person.
                    UIApplication.shared.isIdleTimerDisabled = true
                    model.refreshPermission()
                    await model.boot()
                }
                .onChange(of: scenePhase) { phase in
                    if phase == .active {
                        model.refreshPermission()
                        model.resumeMic()
                    } else {
                        model.pauseMic()
                    }
                }
        }
    }
}
