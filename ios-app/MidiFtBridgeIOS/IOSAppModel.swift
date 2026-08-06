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
    @Published var shutdownResult: String?

    /// Soak test: play every mapping in sequence, endlessly. Same engine
    /// auto-play the Mac app drives from "Loop All (Test)" - the loop runs
    /// entirely inside the engine worker, so it also exercises the clip
    /// switch off the main thread, which is where the crashes live.
    @Published var autoPlay = false

    /// Shown while looping so a soak run can be read off the screen.
    @Published var soakSummary = ""

    private let engine = MFBEngine()

    /// The engine only fires its delegate on discrete state changes, so live
    /// counters (frames sent, position) need polling. Same 0.2s cadence the
    /// Mac app uses.
    private var statusTimer: Timer?

    /// When the current soak run started; nil when not looping.
    private var soakStart: Date?

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

    /// Every engine entry point blocks: thread joins, AVAsset loading, BLE
    /// waits. Run from a tap they blocked the main thread, and iOS kills any
    /// app whose main thread misses a scene update for 10s — that is exactly
    /// the 0x8badf00d crash of 2026-08-04. Serial, so triggers still apply in
    /// tap order.
    private let engineQueue = DispatchQueue(label: "de.welt.mfb.engine-control")

    /// True while a control call is in flight, so the UI can disable the
    /// buttons instead of queueing up a burst of starts and stops.
    @Published var busy = false

    /// Run `body` on the engine queue, then hand `finish` back to the UI.
    private func onEngineQueue(_ name: String,
                               _ body: @escaping () -> Void,
                               finish: @escaping () -> Void = {}) {
        busy = true
        engineQueue.async {
            Diagnostics.shared.span(name, body)
            Task { @MainActor in
                self.busy = false
                finish()
                self.refreshState()
            }
        }
    }

    func start() {
        guard !running, !busy else { return }
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
        status = "Starting…"

        // statusServer: the HTTP status page binds fine on iOS but is only
        // reachable while the app is foregrounded. On for the spike.
        let path = configURL.path
        var ok = false
        onEngineQueue("engine.start",
                      { ok = self.engine.start(withConfigPath: path, statusServer: true) },
                      finish: {
            if ok {
                self.status = "Running"
                self.statusTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                    Task { @MainActor in self?.refreshState() }
                }
            } else {
                self.status = "Engine failed to start - see the log."
                UIApplication.shared.isIdleTimerDisabled = false
            }
        })
    }

    func stop() {
        guard !busy else { return }
        statusTimer?.invalidate()
        statusTimer = nil
        status = "Stopping…"
        onEngineQueue("engine.stop", { self.engine.stop() }, finish: {
            UIApplication.shared.isIdleTimerDisabled = false
            self.previewImage = nil
            self.autoPlay = false
            self.soakStart = nil
            self.status = "Stopped"
        })
    }

    func trigger(index: Int) {
        let name = index < mappings.count ? mappings[index].clip : "#\(index)"
        onEngineQueue("trigger(\(name))") { self.engine.triggerMapping(at: index) }
    }
    func stopClip()    { onEngineQueue("stopClip")    { self.engine.stopActiveClip() } }
    func togglePause() { onEngineQueue("togglePause") { self.engine.togglePause() } }

    /// Start/stop the endless test loop. Engine-side, so it keeps running
    /// with the screen locked (the idle timer is already disabled) and
    /// survives the app being backgrounded for as long as iOS allows.
    func toggleLoop() {
        let want = !autoPlay
        Diagnostics.shared.log("loop: \(want ? "START" : "STOP") soak")
        soakStart = want ? Date() : nil
        onEngineQueue("setAutoPlay") { self.engine.setAutoPlay(want) }
    }

    /// Shut down all FT panels via their HTTP endpoints. Each request blocks up
    /// to 3s, so run it off the main thread and report the per-panel summary.
    func shutdownPanels() {
        shutdownResult = "Shutting down panels…"
        DispatchQueue.global(qos: .userInitiated).async { [engine] in
            let summary = engine.shutdownPanels()
            DispatchQueue.main.async { self.shutdownResult = summary }
        }
    }

    func refreshState() {
        running    = engine.running
        let previousClip = activeClip
        activeClip = engine.activeClipName
        midiDevice = engine.midiDeviceName
        autoPlay   = engine.isAutoPlay()

        // Clip changes are the interesting events in a soak run: the crash
        // signature is a switch, not a steady state.
        if activeClip != previousClip, !activeClip.isEmpty {
            Diagnostics.shared.countClipStart(activeClip)
        }

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

        updateSoakSummary()
    }

    /// One line of soak health, mirrored into every heartbeat in the log so a
    /// crashed run can be read back afterwards.
    private func updateSoakSummary() {
        let frames = panels.map { "\($0.name.prefix(1))=\($0.framesSent)" }.joined(separator: " ")
        let mem = String(format: "%.0fMB", Diagnostics.memoryFootprintMB())
        let clips = Diagnostics.shared.clipStarts

        if let since = soakStart {
            soakSummary = "loop \(Diagnostics.hms(Date().timeIntervalSince(since)))"
                        + " · \(clips) clips · \(mem)"
        } else {
            soakSummary = ""
        }
        Diagnostics.shared.setSnapshot(
            "clip=\(activeClip.isEmpty ? "-" : activeClip) frames[\(frames)] loop=\(autoPlay)")
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
