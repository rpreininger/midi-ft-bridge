// ====================================================================
//  ConfigModel - Swift mirror of config.json, used by the in-app editor.
//
//  The C++ Engine still loads config from a JSON file by path (and the
//  Raspberry Pi deployment reads the same file), so this model is a
//  faithful round-trip of that format: decoding tolerates missing keys
//  (older/hand-written files), and encoding produces clean JSON the
//  simple C++ parser in src/config.h can read back.
// ====================================================================
import Foundation

struct ConfigPanel: Identifiable, Codable, Equatable {
    var id = UUID()
    var name = ""
    var ip = "127.0.0.1"
    var port = 1337
    var srcX = 0
    var srcY = 0
    var srcW = 128
    var srcH = 128
    var maxFps = 0
    var type = "ft"          // "ft" | "ble" | "ble_udp"
    var bleAddr = ""         // BLE MAC (Linux/BlueZ)
    var bleName = ""         // BLE local-name match (macOS/CoreBluetooth)
    var brightness = 80

    var isBle: Bool { type.hasPrefix("ble") }

    // `id` is intentionally absent so it neither decodes nor encodes.
    enum CodingKeys: String, CodingKey {
        case name, ip, port
        case srcX = "src_x", srcY = "src_y", srcW = "src_w", srcH = "src_h"
        case maxFps = "max_fps"
        case type
        case bleAddr = "ble_addr", bleName = "ble_name"
        case brightness
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
        ip = try c.decodeIfPresent(String.self, forKey: .ip) ?? "127.0.0.1"
        port = try c.decodeIfPresent(Int.self, forKey: .port) ?? 1337
        srcX = try c.decodeIfPresent(Int.self, forKey: .srcX) ?? 0
        srcY = try c.decodeIfPresent(Int.self, forKey: .srcY) ?? 0
        srcW = try c.decodeIfPresent(Int.self, forKey: .srcW) ?? 128
        srcH = try c.decodeIfPresent(Int.self, forKey: .srcH) ?? 128
        maxFps = try c.decodeIfPresent(Int.self, forKey: .maxFps) ?? 0
        let t = try c.decodeIfPresent(String.self, forKey: .type) ?? "ft"
        type = t.isEmpty ? "ft" : t
        bleAddr = try c.decodeIfPresent(String.self, forKey: .bleAddr) ?? ""
        bleName = try c.decodeIfPresent(String.self, forKey: .bleName) ?? ""
        brightness = try c.decodeIfPresent(Int.self, forKey: .brightness) ?? 80
    }

    // Hand-rolled so the written file stays close to what a human would
    // type: optional/BLE-only fields are omitted unless they carry meaning.
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(name, forKey: .name)
        try c.encode(ip, forKey: .ip)
        try c.encode(port, forKey: .port)
        try c.encode(srcX, forKey: .srcX)
        try c.encode(srcY, forKey: .srcY)
        try c.encode(srcW, forKey: .srcW)
        try c.encode(srcH, forKey: .srcH)
        if maxFps > 0 { try c.encode(maxFps, forKey: .maxFps) }
        if type != "ft" { try c.encode(type, forKey: .type) }
        if !bleAddr.isEmpty { try c.encode(bleAddr, forKey: .bleAddr) }
        if !bleName.isEmpty { try c.encode(bleName, forKey: .bleName) }
        if isBle { try c.encode(brightness, forKey: .brightness) }
    }
}

struct ConfigMapping: Identifiable, Codable, Equatable {
    var id = UUID()
    var note = 36
    var panel = "all"
    var clip = ""

    enum CodingKeys: String, CodingKey { case note, panel, clip }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        note = try c.decodeIfPresent(Int.self, forKey: .note) ?? 0
        panel = try c.decodeIfPresent(String.self, forKey: .panel) ?? "all"
        clip = try c.decodeIfPresent(String.self, forKey: .clip) ?? ""
    }
}

struct AppConfig: Codable, Equatable {
    var panels: [ConfigPanel] = []
    var clipsDir = "./clips"
    var midiChannel = -1     // 0-15 filter (chan 10 == 9); -1 = any
    var mappings: [ConfigMapping] = []
    var defaultFps = 25
    var videoWidth = 256
    var videoHeight = 128
    var webPort = 8080
    var audioDevice = ""
    var debug = 0            // 0/1 (kept as Int to match the C++ parser)

    // Order chosen so the written file reads like the original config.json.
    enum CodingKeys: String, CodingKey {
        case panels
        case clipsDir = "clips_dir"
        case midiChannel = "midi_channel"
        case mappings
        case defaultFps = "default_fps"
        case videoWidth = "video_width"
        case videoHeight = "video_height"
        case webPort = "web_port"
        case audioDevice = "audio_device"
        case debug
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        panels = try c.decodeIfPresent([ConfigPanel].self, forKey: .panels) ?? []
        clipsDir = try c.decodeIfPresent(String.self, forKey: .clipsDir) ?? "./clips"
        midiChannel = try c.decodeIfPresent(Int.self, forKey: .midiChannel) ?? -1
        mappings = try c.decodeIfPresent([ConfigMapping].self, forKey: .mappings) ?? []
        defaultFps = try c.decodeIfPresent(Int.self, forKey: .defaultFps) ?? 25
        videoWidth = try c.decodeIfPresent(Int.self, forKey: .videoWidth) ?? 256
        videoHeight = try c.decodeIfPresent(Int.self, forKey: .videoHeight) ?? 128
        webPort = try c.decodeIfPresent(Int.self, forKey: .webPort) ?? 8080
        audioDevice = try c.decodeIfPresent(String.self, forKey: .audioDevice) ?? ""
        debug = try c.decodeIfPresent(Int.self, forKey: .debug) ?? 0
    }

    /// Distinct panel names plus the "all" pseudo-target, for mapping pickers.
    var panelTargets: [String] {
        ["all"] + panels.map(\.name).filter { !$0.isEmpty }
    }

    // MARK: - Disk I/O

    static func load(from path: String) throws -> AppConfig {
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        return try JSONDecoder().decode(AppConfig.self, from: data)
    }

    func save(to path: String) throws {
        let enc = JSONEncoder()
        // Sorted keys keep the written file stable across saves (clean git
        // diffs); the C++ parser in src/config.h is order-independent.
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let data = try enc.encode(self)
        try data.write(to: URL(fileURLWithPath: path), options: .atomic)
    }
}
