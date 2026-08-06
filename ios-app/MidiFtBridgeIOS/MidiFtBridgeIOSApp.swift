import SwiftUI

@main
struct MidiFtBridgeIOSApp: App {
    @StateObject private var model = IOSAppModel()

    // Before anything else: the flight recorder has to be up before the
    // engine writes its first line or a signal handler is needed.
    init() { Diagnostics.shared.start() }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}
