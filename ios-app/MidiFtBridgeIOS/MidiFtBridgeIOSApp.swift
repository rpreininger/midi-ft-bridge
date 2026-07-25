import SwiftUI

@main
struct MidiFtBridgeIOSApp: App {
    @StateObject private var model = IOSAppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}
