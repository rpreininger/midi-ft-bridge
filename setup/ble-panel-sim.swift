#!/usr/bin/env swift
//
//  ble-panel-sim.swift — a fake iPixel BLE panel.
//
//  The UDP panel simulator (run-panel-sims.sh) cannot stand in for the BT
//  panel: that one speaks BLE GATT, not UDP/PPM. This makes the Mac advertise
//  itself as the iPixel — same local name, same fa02/fa03 characteristics —
//  reassembles the chunked PNG stream, checks the CRC, ACKs each window and
//  draws the result in the terminal.
//
//  It mirrors src/ble_sender_macos.mm exactly:
//
//    every message  : uint16 LE length (INCLUDING these 2 bytes), then payload
//    PNG window     : 02 00 <option> <len32 LE> <crc32 LE> 00 00 <png bytes>
//                     option 0x00 starts a frame, 0x02 continues it
//                     windows are 12 KB, written in 244-byte chunks
//    brightness     : 04 80 <value>
//    ACK            : notification on fa03, exactly 5 bytes, first byte 0x05
//                     (the sender drops any other length), one per window;
//                     it waits up to 3 s before giving up on each
//
//  Usage:
//      swift setup/ble-panel-sim.swift                    # name from config.json
//      swift setup/ble-panel-sim.swift --name LED_BLE_x   # override
//      swift setup/ble-panel-sim.swift --quiet            # counters only
//      swift setup/ble-panel-sim.swift --selftest         # verify the wire format,
//                                                         # add --render to look
//
//  macOS will ask *the terminal app* for Bluetooth permission the first time.
//  If nothing is ever discovered, check System Settings → Privacy & Security →
//  Bluetooth.
//
//  Note: while this runs, the Mac is advertising the panel's name — so the
//  real BT panel must be off, or the sender may connect to whichever it finds
//  first.
//
import CoreBluetooth
import Foundation
import ImageIO

// Unbuffered: stdout is block-buffered when it is a pipe, so piping this to a
// file or another process would show nothing until it exits.
setvbuf(stdout, nil, _IONBF, 0)

// fa02 = write (central -> panel), fa03 = notify (panel -> central).
// Service UUID is not matched by the sender (it discovers all services and
// looks for these two characteristics), so any container will do.
let SERVICE_UUID = CBUUID(string: "0000fa00-0000-1000-8000-00805f9b34fb")
let WRITE_UUID   = CBUUID(string: "0000fa02-0000-1000-8000-00805f9b34fb")
let NOTIFY_UUID  = CBUUID(string: "0000fa03-0000-1000-8000-00805f9b34fb")

// MARK: - CRC32 (matches binascii.crc32 / the sender's table)

func crc32(_ bytes: [UInt8]) -> UInt32 {
    struct Table { static let t: [UInt32] = (0..<256).map { i -> UInt32 in
        var c = UInt32(i)
        for _ in 0..<8 { c = (c & 1) != 0 ? (0xEDB88320 ^ (c >> 1)) : (c >> 1) }
        return c
    }}
    var crc: UInt32 = 0xFFFFFFFF
    for b in bytes { crc = Table.t[Int((crc ^ UInt32(b)) & 0xFF)] ^ (crc >> 8) }
    return crc ^ 0xFFFFFFFF
}

// MARK: - Arguments

func argValue(_ flag: String) -> String? {
    let a = CommandLine.arguments
    guard let i = a.firstIndex(of: flag), i + 1 < a.count else { return nil }
    return a[i + 1]
}
var quiet = CommandLine.arguments.contains("--quiet")

/// Default the advertised name to whatever config.json's ble panel expects, so
/// the simulator follows the real rig without being told.
func nameFromConfig() -> String {
    let here = URL(fileURLWithPath: CommandLine.arguments[0])
        .deletingLastPathComponent().deletingLastPathComponent()
    for url in [here.appendingPathComponent("config.json"),
                URL(fileURLWithPath: "config.json")] {
        guard let data = try? Data(contentsOf: url),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let panels = json["panels"] as? [[String: Any]] else { continue }
        for p in panels where (p["type"] as? String) == "ble" {
            if let n = p["ble_name"] as? String { return n }
        }
    }
    return "LED_BLE_25F1E13D"
}
let advertisedName = argValue("--name") ?? nameFromConfig()

// MARK: - Panel

final class FakePanel: NSObject, CBPeripheralManagerDelegate {

    private var manager: CBPeripheralManager!
    private var notifyChar: CBMutableCharacteristic!
    private var subscribers = 0

    /// Length-prefixed message reassembly across 244-byte GATT writes.
    private var rx = [UInt8]()

    /// PNG assembly across 12 KB windows.
    private var png = [UInt8]()
    private var expectedLen = 0
    private var expectedCRC: UInt32 = 0

    private var frames = 0
    private var badCRC = 0
    private var brightness = -1
    private var lastReport = Date()
    private var framesInWindow = 0
    private var fps = 0.0
    private var pending = false        // an ACK could not be queued yet

    func start() {
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    /// Push raw GATT-write bytes in, as if they had arrived on fa02. Used by
    /// --selftest to exercise the parser without any Bluetooth hardware.
    func feed(_ bytes: [UInt8]) {
        rx.append(contentsOf: bytes)
        drain()
    }

    var frameCount: Int { frames }
    var crcFailures: Int { badCRC }

    // MARK: Peripheral lifecycle

    func peripheralManagerDidUpdateState(_ p: CBPeripheralManager) {
        switch p.state {
        case .poweredOn:
            let write = CBMutableCharacteristic(
                type: WRITE_UUID,
                properties: [.write, .writeWithoutResponse],
                value: nil, permissions: [.writeable])
            notifyChar = CBMutableCharacteristic(
                type: NOTIFY_UUID, properties: [.notify],
                value: nil, permissions: [.readable])
            let service = CBMutableService(type: SERVICE_UUID, primary: true)
            service.characteristics = [write, notifyChar]
            p.add(service)
            // Name ONLY — no service UUID. A BLE advertisement is 31 bytes; a
            // 128-bit service UUID eats 18 of them and a 16-char local name
            // another 18, so advertising both overflows and the name is
            // truncated or dropped. The bridge matches on the *full* name
            // (substring search for "LED_BLE_…"), so a truncated name means it
            // never recognises us — and it does not filter on service UUID
            // anyway, it discovers services after connecting.
            p.startAdvertising([CBAdvertisementDataLocalNameKey: advertisedName])
            print("advertising as \"\(advertisedName)\" — waiting for the bridge…")
        case .poweredOff:
            print("Bluetooth is off")
        case .unauthorized:
            print("Bluetooth permission denied — System Settings → Privacy & Security → Bluetooth")
        case .unsupported:
            print("BLE peripheral role unsupported on this Mac")
        default:
            break
        }
    }

    /// Advertising can fail silently otherwise — most often because the
    /// payload does not fit in the 31-byte advertisement.
    func peripheralManagerDidStartAdvertising(_ p: CBPeripheralManager, error: Error?) {
        if let e = error { print("ADVERTISING FAILED: \(e.localizedDescription)") }
        else { print("advertising is live") }
    }

    func peripheralManager(_ p: CBPeripheralManager,
                           didAdd service: CBService, error: Error?) {
        if let e = error { print("service registration failed: \(e.localizedDescription)") }
    }

    func peripheralManager(_ p: CBPeripheralManager,
                           central: CBCentral,
                           didSubscribeTo characteristic: CBCharacteristic) {
        subscribers += 1
        // The sender treats "notifications on" as the handshake completing.
        print("bridge connected (MTU \(central.maximumUpdateValueLength) bytes) — ready")
    }

    func peripheralManager(_ p: CBPeripheralManager,
                           central: CBCentral,
                           didUnsubscribeFrom characteristic: CBCharacteristic) {
        subscribers = max(0, subscribers - 1)
        print("bridge disconnected")
        rx.removeAll(); png.removeAll()
    }

    func peripheralManager(_ p: CBPeripheralManager,
                           didReceiveWrite requests: [CBATTRequest]) {
        for r in requests {
            if let v = r.value { rx.append(contentsOf: [UInt8](v)) }
        }
        // Only the first request is answered — that is the documented contract.
        if let first = requests.first { p.respond(to: first, withResult: .success) }
        drain()
    }

    /// Retry an ACK that could not be queued earlier.
    func peripheralManagerIsReady(toUpdateSubscribers p: CBPeripheralManager) {
        if pending { pending = false; sendAck() }
    }

    // MARK: Protocol

    private func drain() {
        while rx.count >= 2 {
            let len = Int(rx[0]) | (Int(rx[1]) << 8)
            guard len >= 2 else { rx.removeAll(); return }   // desync: drop
            guard rx.count >= len else { return }            // wait for the rest
            let payload = Array(rx[2..<len])
            rx.removeFirst(len)
            handle(payload)
        }
    }

    private func handle(_ payload: [UInt8]) {
        guard let kind = payload.first else { return }

        if kind == 0x02, payload.count >= 13 {
            let option = payload[2]
            let total  = UInt32(payload[3]) | UInt32(payload[4]) << 8
                       | UInt32(payload[5]) << 16 | UInt32(payload[6]) << 24
            let crc    = UInt32(payload[7]) | UInt32(payload[8]) << 8
                       | UInt32(payload[9]) << 16 | UInt32(payload[10]) << 24
            if option == 0x00 {                    // first window of a frame
                png.removeAll(keepingCapacity: true)
                expectedLen = Int(total)
                expectedCRC = crc
            }
            png.append(contentsOf: payload[13...])
            sendAck()                              // one ACK per window
            if png.count >= expectedLen && expectedLen > 0 {
                complete()
            }
        } else if kind == 0x04, payload.count >= 3 {
            brightness = Int(payload[2])
            print("brightness set to \(brightness)")
        }
    }

    private func complete() {
        let data = Array(png.prefix(expectedLen))
        png.removeAll(keepingCapacity: true)
        frames += 1
        framesInWindow += 1
        let ok = crc32(data) == expectedCRC
        if !ok { badCRC += 1 }

        let now = Date()
        let elapsed = now.timeIntervalSince(lastReport)
        if elapsed >= 1.0 {
            fps = Double(framesInWindow) / elapsed
            framesInWindow = 0
            lastReport = now
        }

        if quiet {
            print(String(format: "frame %d  %d B  %.1f fps  crc %@  bad=%d",
                         frames, data.count, fps, ok ? "ok" : "BAD", badCRC))
        } else {
            render(png: Data(data), ok: ok, bytes: data.count)
        }
    }

    private func sendAck() {
        guard subscribers > 0, notifyChar != nil else { return }
        // MUST be exactly 5 bytes. The sender ignores anything else outright:
        //     if (val.length == 5) { if (b[0] == 0x05) signal(_ackSema); }
        // A 1-byte 0x05 is silently dropped, and every window then waits out
        // the full 3s ACK timeout (~3.5s per frame, measured).
        // It reads like the same framing as everything else: uint16 LE length
        // of 5, then three payload bytes it does not inspect.
        if !manager.updateValue(Data([0x05, 0x00, 0x00, 0x00, 0x00]),
                                for: notifyChar, onSubscribedCentrals: nil) {
            pending = true          // queue full — retried from the ready callback
        }
    }

    // MARK: Rendering

    private func render(png data: Data, ok: Bool, bytes: Int) {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil),
              let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            print("frame \(frames): PNG decode failed")
            return
        }
        let w = img.width, h = img.height
        // Own the pixel buffer explicitly: passing `&array` to CGContext only
        // keeps the pointer alive for that one call, so the draw would land in
        // a temporary and every pixel would read back black.
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: w * h * 4)
        buf.initialize(repeating: 0, count: w * h * 4)
        defer { buf.deallocate() }
        guard let ctx = CGContext(data: buf, width: w, height: h,
                                  bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))

        func px(_ x: Int, _ y: Int) -> (Int, Int, Int) {
            let i = (y * w + x) * 4
            return (Int(buf[i]), Int(buf[i + 1]), Int(buf[i + 2]))
        }

        // Two pixel rows per character line, via the upper half block.
        var out = "\u{1B}[H"
        out += "iPixel \(advertisedName) — \(w)x\(h)\n"
        for y in stride(from: 0, to: h - 1, by: 2) {
            for x in 0..<w {
                let t = px(x, y), b = px(x, y + 1)
                out += "\u{1B}[38;2;\(t.0);\(t.1);\(t.2)m\u{1B}[48;2;\(b.0);\(b.1);\(b.2)m▀"
            }
            out += "\u{1B}[0m\n"
        }
        out += String(format: "\u{1B}[2Kframe %d  %d B  %.1f fps  crc %@  bad=%d  brightness=%@\n",
                      frames, bytes, fps, ok ? "ok" : "BAD", badCRC,
                      brightness < 0 ? "-" : "\(brightness)")
        FileHandle.standardOutput.write(out.data(using: .utf8)!)
    }
}

func CGColorSpaceDeviceRGB() -> CGColorSpace { CGColorSpaceCreateDeviceRGB() }

// MARK: - Self-test
//
// Replays exactly what BleSender::sendPng() puts on the wire — 12 KB windows,
// 244-byte chunks, the same 13-byte header — so the parser can be verified
// without a phone or a panel. Run: swift setup/ble-panel-sim.swift --selftest

func encodePNG(rgb: [UInt8], w: Int, h: Int) -> Data? {
    let rgba = UnsafeMutablePointer<UInt8>.allocate(capacity: w * h * 4)
    defer { rgba.deallocate() }
    for i in 0..<(w * h) {
        rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1]
        rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255
    }
    guard let ctx = CGContext(data: rgba, width: w, height: h, bitsPerComponent: 8,
                              bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
          let img = ctx.makeImage() else { return nil }
    let out = NSMutableData()
    guard let dst = CGImageDestinationCreateWithData(out, "public.png" as CFString, 1, nil)
    else { return nil }
    CGImageDestinationAddImage(dst, img, nil)
    guard CGImageDestinationFinalize(dst) else { return nil }
    return out as Data
}

func selftest() -> Int32 {
    // counters, not a picture — unless --render is asked for
    quiet = !CommandLine.arguments.contains("--render")
    let w = 32, h = 16
    var rgb = [UInt8](repeating: 0, count: w * h * 3)
    for y in 0..<h { for x in 0..<w {
        let i = (y * w + x) * 3
        rgb[i] = UInt8(x * 8 % 256); rgb[i + 1] = UInt8(y * 16 % 256); rgb[i + 2] = 128
    }}
    guard let png = encodePNG(rgb: rgb, w: w, h: h) else {
        print("selftest: could not build a PNG"); return 1
    }
    let bytes = [UInt8](png)
    let crc = crc32(bytes)
    let total = UInt32(bytes.count)
    print("selftest: PNG \(bytes.count) B, crc \(String(format: "%08x", crc))")

    let panel = FakePanel()
    var offset = 0, first = true
    let windowSize = 12 * 1024, chunkSize = 244
    while offset < bytes.count {
        let n = min(windowSize, bytes.count - offset)
        var window: [UInt8] = [0x02, 0x00, first ? 0x00 : 0x02]
        first = false
        for shift in stride(from: 0, to: 32, by: 8) { window.append(UInt8((total >> UInt32(shift)) & 0xFF)) }
        for shift in stride(from: 0, to: 32, by: 8) { window.append(UInt8((crc   >> UInt32(shift)) & 0xFF)) }
        window.append(0x00); window.append(0x00)
        window.append(contentsOf: bytes[offset..<(offset + n)])

        let len = UInt16(window.count + 2)
        var full: [UInt8] = [UInt8(len & 0xFF), UInt8(len >> 8)]
        full.append(contentsOf: window)

        var i = 0
        while i < full.count {           // split across GATT writes, as the sender does
            let c = min(chunkSize, full.count - i)
            panel.feed(Array(full[i..<(i + c)]))
            i += c
        }
        offset += n
    }

    // And a brightness command: {5,0,4,0x80,80}
    panel.feed([5, 0, 4, 0x80, 80])

    let ok = panel.frameCount == 1 && panel.crcFailures == 0
    print("selftest: frames=\(panel.frameCount) crcFailures=\(panel.crcFailures) -> \(ok ? "PASS" : "FAIL")")
    return ok ? 0 : 1
}

if CommandLine.arguments.contains("--selftest") {
    exit(selftest())
}

if !quiet { print("\u{1B}[2J", terminator: "") }
let panel = FakePanel()
panel.start()
RunLoop.main.run()
