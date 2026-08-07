import SwiftUI

@main
struct MidiFtBridgeApp: App {
    @StateObject private var model = AppModel()

    // Before anything else: launched as a .app from Finder, stderr goes
    // nowhere, so without this a misbehaving show leaves nothing to read.
    // Log lands in ~/Library/Logs/MIDI-FT Bridge/mfb.log.
    init() { Diagnostics.shared.start() }

    var body: some Scene {
        WindowGroup("MIDI-FT Bridge") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 720, minHeight: 480)
        }
    }
}
