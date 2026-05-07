import SwiftUI

@main
struct MidiFtBridgeApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("MIDI-FT Bridge") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 720, minHeight: 480)
        }
    }
}
