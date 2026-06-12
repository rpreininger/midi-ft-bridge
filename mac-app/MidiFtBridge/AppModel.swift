import AppKit
import Combine
import SwiftUI

struct PanelInfo: Identifiable {
    let id = UUID()
    let name: String
    let x: Int
    let y: Int
    let width: Int
    let height: Int
    let type: String
}

struct MappingInfo: Identifiable {
    let id = UUID()
    let index: Int
    let note: Int
    let clip: String
    let panel: String
}

/// Live per-panel status mirrored from the engine each tick.
struct PanelStatusInfo: Identifiable {
    var id: String { name }
    let name: String
    let ip: String
    let port: Int
    let framesSent: UInt64
    let bytesSent: UInt64
    let connected: Bool
    let activeClip: String
    let type: String

    /// Only FT panels with a real IP can be SSH shut down.
    var canShutdown: Bool { type == "ft" && !ip.isEmpty && ip != "127.0.0.1" }
    var isPlaying: Bool { !activeClip.isEmpty }
}

final class AppModel: NSObject, ObservableObject {
    private let engine = MFBEngine()

    @Published var running = false
    @Published var activeClipName = ""
    @Published var clipPaused = false
    @Published var autoPlay = false
    @Published var midiDeviceName = ""
    @Published var canvasSize = NSSize(width: 0, height: 0)
    @Published var panels: [PanelInfo] = []
    @Published var panelStatus: [PanelStatusInfo] = []
    @Published var mappings: [MappingInfo] = []
    @Published var selectedIndex: Int? = nil
    @Published var previewImage: NSImage?
    @Published var panelImages: [String: NSImage] = [:]
    @Published var lastError: String?

    // In-app config editor: a working copy is held here while the sheet is up.
    @Published var showConfigEditor = false
    @Published var editingConfig = AppConfig()

    private static let configPathKey = "MFBConfigPath"

    @Published var configPath: String = {
        // Restore previously-used path; otherwise scan likely locations.
        if let saved = UserDefaults.standard.string(forKey: configPathKey),
           FileManager.default.fileExists(atPath: saved) {
            return saved
        }
        let cwd = FileManager.default.currentDirectoryPath
        for c in ["config_local.json", "config.json"] {
            let p = "\(cwd)/\(c)"
            if FileManager.default.fileExists(atPath: p) { return p }
        }
        return ""
    }() {
        didSet { UserDefaults.standard.set(configPath, forKey: Self.configPathKey) }
    }

    private var statusTimer: Timer?

    override init() {
        super.init()
        engine.delegate = self
        // Poll engine state so the UI catches transitions the bridge can't
        // signal directly (e.g. a clip auto-finishing inside the worker).
        statusTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            self?.refreshState()
        }
    }

    deinit {
        statusTimer?.invalidate()
    }

    /// Show an open panel and let the user pick a config JSON file.
    func chooseConfigFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.message = "Select a config_local.json"
        if !configPath.isEmpty {
            panel.directoryURL = URL(fileURLWithPath: configPath).deletingLastPathComponent()
        }
        if panel.runModal() == .OK, let url = panel.url {
            configPath = url.path
        }
    }

    // MARK: - In-app config editor

    /// Load the config at `configPath` (or start a fresh template) and open
    /// the editor sheet on a working copy.
    func openConfigEditor() {
        lastError = nil
        if !configPath.isEmpty, FileManager.default.fileExists(atPath: configPath) {
            do {
                editingConfig = try AppConfig.load(from: configPath)
            } catch {
                lastError = "Couldn't read config: \(error.localizedDescription)"
                editingConfig = AppConfig()
            }
        } else {
            editingConfig = AppConfig()   // fresh config; Save will prompt for a path
        }
        showConfigEditor = true
    }

    /// Persist the working copy to `configPath`, prompting for a location if
    /// none is set yet. Leaves the editor open if the user cancels the prompt.
    func saveConfigEditor() {
        if configPath.isEmpty, let chosen = promptSaveLocation() {
            configPath = chosen
        }
        guard !configPath.isEmpty else { return }   // user cancelled the save panel
        do {
            try editingConfig.save(to: configPath)
            showConfigEditor = false
            // If the engine is live it keeps the previously-loaded config until
            // restarted; the editor footer already warns about this.
        } catch {
            lastError = "Couldn't write config: \(error.localizedDescription)"
        }
    }

    func cancelConfigEditor() {
        showConfigEditor = false
    }

    /// Ask the user where to save a brand-new config file.
    private func promptSaveLocation() -> String? {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "config_local.json"
        panel.message = "Save configuration as…"
        panel.directoryURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        return panel.runModal() == .OK ? panel.url?.path : nil
    }

    func start() {
        lastError = nil

        // Resolve config path and chdir into its directory so the engine's
        // relative paths (clips_dir like "./clips") resolve correctly when
        // launched as a .app (where cwd is "/" by default).
        let configURL = URL(fileURLWithPath: configPath).standardizedFileURL
        let dir = configURL.deletingLastPathComponent().path
        if FileManager.default.fileExists(atPath: dir) {
            FileManager.default.changeCurrentDirectoryPath(dir)
        }

        guard engine.start(withConfigPath: configURL.path, statusServer: true) else {
            lastError = "Failed to load config from \(configURL.path)"
            return
        }
        refreshState()

        let panelDicts = engine.panels()
        panels = panelDicts.map {
            PanelInfo(
                name: ($0["name"] as? String) ?? "",
                x: ($0["x"] as? Int) ?? 0,
                y: ($0["y"] as? Int) ?? 0,
                width: ($0["width"] as? Int) ?? 0,
                height: ($0["height"] as? Int) ?? 0,
                type: ($0["type"] as? String) ?? "ft"
            )
        }
        let mapDicts = engine.mappings()
        mappings = mapDicts.enumerated().map { (idx, d) in
            MappingInfo(
                index: idx,
                note: (d["note"] as? Int) ?? 0,
                clip: (d["clip"] as? String) ?? "",
                panel: (d["panel"] as? String) ?? ""
            )
        }
        canvasSize = NSSize(width: engine.canvasWidth, height: engine.canvasHeight)
    }

    func stop() {
        engine.stop()
        previewImage = nil
        panelStatus = []
        refreshState()
    }

    func triggerMapping(_ idx: Int) { engine.triggerMapping(at: idx) }
    func stopClip()                 { engine.stopActiveClip() }
    func togglePause()              { engine.togglePause() }

    /// Toggle test mode: play every mapping in sequence, looping endlessly.
    func toggleAutoPlay() {
        engine.setAutoPlay(!autoPlay)
        autoPlay = engine.isAutoPlay()
    }

    /// SSH `sudo shutdown now` to all FT panels.
    func shutdownAllPanels() { engine.shutdownPanels() }

    /// SSH `sudo shutdown now` to a single FT panel by name.
    func shutdownPanel(_ name: String) { engine.shutdownPanelNamed(name) }

    /// Move selection one row down (snaps to first row if nothing selected).
    func selectNext() {
        guard !mappings.isEmpty else { selectedIndex = nil; return }
        selectedIndex = min((selectedIndex ?? -1) + 1, mappings.count - 1)
    }

    /// Move selection one row up (snaps to first row if nothing selected).
    func selectPrevious() {
        guard !mappings.isEmpty else { selectedIndex = nil; return }
        if let cur = selectedIndex { selectedIndex = max(cur - 1, 0) }
        else { selectedIndex = 0 }
    }

    /// Trigger the currently-selected mapping (if any and engine running).
    func triggerSelected() {
        guard let idx = selectedIndex, idx < mappings.count else { return }
        if running { triggerMapping(idx) }
    }

    /// Map a single character to a mapping index (matches CLI --test mode).
    /// Returns true if handled (caller should consume the key event).
    /// Digit/dash keys also update `selectedIndex` so the highlighted row
    /// follows the trigger source.
    @discardableResult
    func handleKey(_ char: String) -> Bool {
        let idx: Int?
        switch char {
        case "1": idx = 0
        case "2": idx = 1
        case "3": idx = 2
        case "4": idx = 3
        case "5": idx = 4
        case "6": idx = 5
        case "7": idx = 6
        case "8": idx = 7
        case "9": idx = 8
        case "0": idx = 9
        case "-": idx = 10
        case " ":
            if running, !activeClipName.isEmpty { togglePause() }
            return true   // always consume to avoid system beep
        case "\u{1B}": // Escape
            if running, !activeClipName.isEmpty { stopClip() }
            return true
        default: idx = nil
        }
        guard let i = idx, i < mappings.count else { return false }
        selectedIndex = i
        if running { triggerMapping(i) }
        return true
    }

    /// Returns a short label for the keyboard binding of mapping at `index`.
    static func keyHint(for index: Int) -> String {
        switch index {
        case 0...8:  return "\(index + 1)"
        case 9:      return "0"
        case 10:     return "-"
        default:     return ""
        }
    }

    private func refreshState() {
        // Idempotent: only assign when changed so the polling timer doesn't
        // republish unchanged values to dependent SwiftUI views every tick.
        let r = engine.running
        if running != r { running = r }
        let n = engine.activeClipName
        if activeClipName != n { activeClipName = n }
        let m = engine.midiDeviceName
        if midiDeviceName != m { midiDeviceName = m }
        let p = engine.isClipPaused()
        if clipPaused != p { clipPaused = p }
        let ap = engine.isAutoPlay()
        if autoPlay != ap { autoPlay = ap }

        // Mirror live panel status while running (frames/bytes change every
        // tick, so this republishes by design). Cleared on stop().
        if r {
            panelStatus = engine.panelStatus().map { d in
                PanelStatusInfo(
                    name:       (d["name"] as? String) ?? "",
                    ip:         (d["ip"] as? String) ?? "",
                    port:       (d["port"] as? Int) ?? 0,
                    framesSent: (d["framesSent"] as? NSNumber)?.uint64Value ?? 0,
                    bytesSent:  (d["bytesSent"] as? NSNumber)?.uint64Value ?? 0,
                    connected:  (d["connected"] as? Bool) ?? false,
                    activeClip: (d["activeClip"] as? String) ?? "",
                    type:       (d["type"] as? String) ?? "ft"
                )
            }
        }
    }
}

extension AppModel: MFBEngineDelegate {
    func engine(_ engine: MFBEngine,
                didProduceRGBAFrame rgba: Data,
                width: Int,
                height: Int) {
        guard let canvas = AppModel.makeCGImage(rgba: rgba, width: width, height: height) else {
            return
        }
        previewImage = NSImage(cgImage: canvas,
                               size: NSSize(width: width, height: height))

        // Crop the canvas into per-panel tiles, replacing panel_viewer.
        var tiles: [String: NSImage] = [:]
        for panel in panels {
            let rect = CGRect(x: panel.x, y: panel.y,
                              width: panel.width, height: panel.height)
            if let cropped = canvas.cropping(to: rect) {
                tiles[panel.name] = NSImage(
                    cgImage: cropped,
                    size: NSSize(width: panel.width, height: panel.height)
                )
            }
        }
        panelImages = tiles
    }

    func engineStateDidChange(_ engine: MFBEngine) {
        refreshState()
    }

    /// Build a CGImage from RGBA8 pixel data. The CGImage retains the data via
    /// CGDataProvider so the caller can drop the reference safely.
    private static func makeCGImage(rgba: Data, width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0 else { return nil }
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }

        let bitmapInfo = CGBitmapInfo(rawValue:
            CGImageAlphaInfo.noneSkipLast.rawValue
            | CGBitmapInfo.byteOrder32Big.rawValue
        )
        return CGImage(
            width: width, height: height,
            bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: bitmapInfo,
            provider: provider, decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }
}
