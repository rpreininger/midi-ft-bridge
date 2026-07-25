// ====================================================================
//  IOSAppModel - engine lifecycle for the iOS spike.
//
//  Deliberately thinner than the Mac AppModel: no config editor, no
//  playlist, no panel shutdown. The point is to prove the shared engine
//  runs on iOS, so this covers config discovery, start/stop, clip
//  triggering and the preview frame path - nothing else.
// ====================================================================
import Combine
import SwiftUI
import UIKit

// Ids are stable (index / name) rather than fresh UUIDs: refreshState runs
// on a timer, and regenerated ids would make SwiftUI rebuild every row.
struct MappingInfo: Identifiable {
    let id: Int          // index into the engine's mapping list
    let note: Int
    let clip: String
    let panel: String
}

struct PanelInfo: Identifiable {
    var id: String { name }
    let name: String
    let ip: String
    let framesSent: UInt64
    let connected: Bool
}

@MainActor
final class IOSAppModel: NSObject, ObservableObject {

    @Published var running = false
    @Published var activeClip = ""
    @Published var midiDevice = ""
    @Published var audioRoute = ""
    @Published var previewImage: UIImage?
    @Published var mappings: [MappingInfo] = []
    @Published var panels: [PanelInfo] = []
    @Published var status = "Idle"

    private let engine = MFBEngine()

    /// The engine only fires its delegate on discrete state changes, so live
    /// counters (frames sent, position) need polling. Same 0.2s cadence the
    /// Mac app uses.
    private var statusTimer: Timer?

    /// Where clips and config.json live. Exposed over Finder / the Files app
    /// via UIFileSharingEnabled, since the clip library is far too large to
    /// ship inside the bundle.
    static var documentsURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    var configURL: URL { Self.documentsURL.appendingPathComponent("config.json") }

    var configExists: Bool { FileManager.default.fileExists(atPath: configURL.path) }

    override init() {
        super.init()
        engine.delegate = self
    }

    func start() {
        guard !running else { return }
        guard configExists else {
            status = "No config.json in Documents. Copy config.json and the "
                   + "clips folder into the app's Documents directory."
            return
        }

        // The engine resolves clips_dir ("./clips") relative to the working
        // directory, exactly as it does on macOS. chdir is permitted inside
        // the iOS sandbox, so the same mechanism works unchanged.
        FileManager.default.changeCurrentDirectoryPath(Self.documentsURL.path)

        // A dropped frame is cosmetic; a locked screen mid-set is not.
        UIApplication.shared.isIdleTimerDisabled = true

        // statusServer: the HTTP status page binds fine on iOS but is only
        // reachable while the app is foregrounded. On for the spike.
        if engine.start(withConfigPath: configURL.path, statusServer: true) {
            status = "Running"
            statusTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.refreshState() }
            }
        } else {
            status = "Engine failed to start - check the Xcode console."
            UIApplication.shared.isIdleTimerDisabled = false
        }
        refreshState()
    }

    func stop() {
        statusTimer?.invalidate()
        statusTimer = nil
        engine.stop()
        UIApplication.shared.isIdleTimerDisabled = false
        previewImage = nil
        status = "Stopped"
        refreshState()
    }

    func trigger(index: Int) { engine.triggerMapping(at: index) }
    func stopClip()          { engine.stopActiveClip() }
    func togglePause()       { engine.togglePause() }

    func refreshState() {
        running    = engine.running
        activeClip = engine.activeClipName
        midiDevice = engine.midiDeviceName

        // On iOS this is the live output route, not a user choice - the
        // system owns routing. See audio_output_macos.mm.
        audioRoute = MFBEngine.availableAudioOutputs().first ?? "unknown"

        mappings = engine.mappings().enumerated().map { index, d in
            MappingInfo(id:    index,
                        note:  (d["note"] as? Int) ?? 0,
                        clip:  (d["clip"] as? String) ?? "",
                        panel: (d["panel"] as? String) ?? "")
        }

        panels = engine.panelStatus().map { d in
            PanelInfo(name:       (d["name"] as? String) ?? "",
                      ip:         (d["ip"] as? String) ?? "",
                      framesSent: (d["framesSent"] as? NSNumber)?.uint64Value ?? 0,
                      connected:  (d["connected"] as? Bool) ?? false)
        }
    }
}

extension IOSAppModel: MFBEngineDelegate {

    nonisolated func engine(_ engine: MFBEngine,
                            didProduceRGBAFrame rgba: Data,
                            width: Int,
                            height: Int) {
        guard let cg = Self.makeCGImage(rgba: rgba, width: width, height: height) else { return }
        Task { @MainActor in self.previewImage = UIImage(cgImage: cg) }
    }

    nonisolated func engineStateDidChange(_ engine: MFBEngine) {
        Task { @MainActor in self.refreshState() }
    }

    /// Canvas frames arrive as RGBA8 with the alpha byte unused - same layout
    /// the Mac app consumes, so the bitmap description matches it exactly.
    /// nonisolated: called straight from the engine's frame callback, off the
    /// main actor, so the conversion does not bounce through a hop per frame.
    private nonisolated static func makeCGImage(rgba: Data, width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0, rgba.count >= width * height * 4 else { return nil }
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue)
        return CGImage(width: width,
                       height: height,
                       bitsPerComponent: 8,
                       bitsPerPixel: 32,
                       bytesPerRow: width * 4,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: bitmapInfo,
                       provider: provider,
                       decode: nil,
                       shouldInterpolate: false,
                       intent: .defaultIntent)
    }
}
