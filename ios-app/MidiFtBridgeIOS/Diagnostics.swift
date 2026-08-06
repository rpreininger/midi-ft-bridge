// ====================================================================
//  Diagnostics - on-device flight recorder.
//
//  The rehearsal crashes of 2026-08-06 left nothing to read: the engine
//  logs to std::cerr, which on a device goes nowhere unless Xcode is
//  attached. This writes everything to a file in Documents instead, so
//  the log survives the crash and can be pulled over the Files app,
//  AirDrop (ShareLink in the UI) or devicectl.
//
//  What it records:
//    * every line the C++ engine writes to stdout/stderr (captured via
//      a pipe, still mirrored to the Xcode console when attached),
//    * app lifecycle, memory warnings, AVAudioSession interruptions,
//      route changes and media-services resets,
//    * a heartbeat with memory footprint / thermal state / uptime,
//    * MAIN-THREAD STALLS. Both known crash modes end with the main
//      thread blocked in a thread join, and a watchdog kill (0x8badf00d)
//      leaves no clue about what it was blocked *in*. `span()` brackets
//      the blocking calls, so the log names the culprit before the kill.
//    * a last-breath line from a signal handler on SIGSEGV/SIGBUS/etc.
//
//  Log file: Documents/logs/mfb.log, previous run kept as mfb.log.1
//  (rotated at launch, and mid-run when it passes MAX_BYTES).
// ====================================================================
import AVFoundation
import Darwin
import Foundation
import UIKit

// The signal handler may only touch async-signal-safe state: a raw fd and
// pre-rendered C buffers, both set up long before anything can crash.
private var gCrashFD: Int32 = -1
private var gCrashMessages: [Int32: (UnsafeMutablePointer<CChar>, Int)] = [:]

private func mfbSignalHandler(_ sig: Int32) {
    if gCrashFD >= 0, let (buf, len) = gCrashMessages[sig] {
        _ = write(gCrashFD, buf, len)
        fsync(gCrashFD)
    }
    signal(sig, SIG_DFL)
    raise(sig)
}

final class Diagnostics {

    static let shared = Diagnostics()

    /// Rotate once the log passes this; a soak run writes a few MB an hour.
    private static let MAX_BYTES: UInt64 = 8 * 1024 * 1024

    private let queue = DispatchQueue(label: "de.welt.mfb.diagnostics")
    private var fd: Int32 = -1
    private var written: UInt64 = 0
    private let started = Date()

    /// Last time the main thread was seen alive, and what it is busy with.
    /// Read from the diagnostics queue, written from the main thread - both
    /// under `stateLock` because a stalled main thread is exactly the case
    /// we need to read consistently.
    private let stateLock = NSLock()
    private var lastMainPing = Date()
    private var currentSpan: (name: String, since: Date)?
    private var reportedStall = false

    /// Soak counters, shown in the UI and in every heartbeat line.
    private(set) var clipStarts = 0

    static var logURL: URL {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("logs", isDirectory: true)
        return dir.appendingPathComponent("mfb.log")
    }

    // MARK: - Setup

    /// Call once, as early as possible - before the engine exists.
    func start() {
        openLogFile()
        installSignalHandlers()
        captureStdErr()
        observeSystemEvents()
        startMainThreadPing()
        startHeartbeat()

        let dev = UIDevice.current
        log("=== MIDI-FT Bridge \(Self.appVersion) — \(dev.model) iOS \(dev.systemVersion) ===")
        log("log file: \(Self.logURL.path)")
    }

    private static var appVersion: String {
        let info = Bundle.main.infoDictionary
        let short = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }

    private func openLogFile() {
        let url = Self.logURL
        let fm = FileManager.default
        try? fm.createDirectory(at: url.deletingLastPathComponent(),
                                withIntermediateDirectories: true)
        rotate(at: url)
        fd = open(url.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
        gCrashFD = fd
    }

    /// Keep exactly one previous run. Two files is enough to compare a crashed
    /// run against the one before it, and the clips already fill the device.
    private func rotate(at url: URL) {
        let fm = FileManager.default
        guard fm.fileExists(atPath: url.path) else { return }
        let old = url.appendingPathExtension("1")
        try? fm.removeItem(at: old)
        try? fm.moveItem(at: url, to: old)
    }

    private func installSignalHandlers() {
        let fatal: [(Int32, String)] = [
            (SIGSEGV, "SIGSEGV"), (SIGBUS, "SIGBUS"), (SIGILL, "SIGILL"),
            (SIGABRT, "SIGABRT"), (SIGFPE, "SIGFPE"), (SIGTRAP, "SIGTRAP"),
        ]
        for (sig, name) in fatal {
            // Render the message now: strdup/format inside a handler is not
            // async-signal-safe, so nothing may be built at crash time.
            let text = "\n*** FATAL \(name) — process died here ***\n"
            let bytes = Array(text.utf8CString)
            let buf = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
            buf.update(from: bytes, count: bytes.count)
            gCrashMessages[sig] = (buf, bytes.count - 1)   // drop the NUL
            signal(sig, mfbSignalHandler)
        }

        NSSetUncaughtExceptionHandler { exc in
            Diagnostics.shared.log("*** UNCAUGHT \(exc.name.rawValue): \(exc.reason ?? "")")
            for frame in exc.callStackSymbols { Diagnostics.shared.log("    \(frame)") }
        }
    }

    /// Route the engine's std::cerr / printf into the log file. The original
    /// stderr is kept and mirrored to, so the Xcode console still works.
    private func captureStdErr() {
        let originalErr = dup(STDERR_FILENO)
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { return }
        let readFD = fds[0], writeFD = fds[1]
        dup2(writeFD, STDERR_FILENO)
        dup2(writeFD, STDOUT_FILENO)
        setvbuf(stdout, nil, _IOLBF, 0)

        Thread.detachNewThread { [weak self] in
            Thread.current.name = "mfb.stderr-capture"
            var pending = Data()
            var chunk = [UInt8](repeating: 0, count: 4096)
            while true {
                let n = read(readFD, &chunk, chunk.count)
                if n <= 0 { break }
                _ = chunk.withUnsafeBytes { Darwin.write(originalErr, $0.baseAddress!, n) }
                pending.append(contentsOf: chunk[0..<n])
                // Timestamp per line, so engine output interleaves correctly
                // with the app's own events.
                while let nl = pending.firstIndex(of: 0x0A) {
                    let line = String(decoding: pending[..<nl], as: UTF8.self)
                    pending.removeSubrange(...nl)
                    if !line.isEmpty { self?.log(line, source: "engine") }
                }
            }
        }
    }

    private func observeSystemEvents() {
        let nc = NotificationCenter.default
        let app: [(Notification.Name, String)] = [
            (UIApplication.didEnterBackgroundNotification, "app: entered background"),
            (UIApplication.willEnterForegroundNotification, "app: entering foreground"),
            (UIApplication.didBecomeActiveNotification, "app: active"),
            (UIApplication.willResignActiveNotification, "app: resigning active"),
            (UIApplication.willTerminateNotification, "app: WILL TERMINATE"),
            (UIApplication.didReceiveMemoryWarningNotification, "app: MEMORY WARNING"),
        ]
        for (name, text) in app {
            nc.addObserver(forName: name, object: nil, queue: nil) { [weak self] _ in
                self?.log(text)
            }
        }

        // An interruption (call, alarm, another app) stops the AudioQueue -
        // which is the clip clock, so playback stalls without any error.
        nc.addObserver(forName: AVAudioSession.interruptionNotification,
                       object: nil, queue: nil) { [weak self] note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt ?? 0
            let began = AVAudioSession.InterruptionType(rawValue: raw) == .began
            self?.log("audio: interruption \(began ? "BEGAN" : "ended")")
        }
        nc.addObserver(forName: AVAudioSession.routeChangeNotification,
                       object: nil, queue: nil) { [weak self] note in
            let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt ?? 0
            let outputs = AVAudioSession.sharedInstance().currentRoute.outputs
                .map(\.portName).joined(separator: ",")
            self?.log("audio: route change (reason \(raw)) -> \(outputs.isEmpty ? "none" : outputs)")
        }
        // mediaserverd died: every AudioQueue and AVAssetReader is now invalid.
        nc.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification,
                       object: nil, queue: nil) { [weak self] _ in
            self?.log("audio: MEDIA SERVICES WERE RESET — audio stack is dead until restart")
        }
    }

    // MARK: - Main-thread stall detection

    private var pingTimer: DispatchSourceTimer?
    private var heartbeatTimer: DispatchSourceTimer?

    private func startMainThreadPing() {
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now(), repeating: .milliseconds(250))
        t.setEventHandler { [weak self] in
            guard let self else { return }
            self.stateLock.lock()
            self.lastMainPing = Date()
            let wasStalled = self.reportedStall
            self.reportedStall = false
            self.stateLock.unlock()
            if wasStalled { self.log("main thread: recovered") }
        }
        t.resume()
        pingTimer = t
    }

    /// Bracket a call that may block. Only a span running *on the main thread*
    /// registers as the stall culprit — engine control now runs on its own
    /// queue, where blocking is expected and harmless.
    @discardableResult
    func span<T>(_ name: String, _ body: () throws -> T) rethrows -> T {
        let begin = Date()
        let onMain = Thread.isMainThread
        if onMain { stateLock.lock(); currentSpan = (name, begin); stateLock.unlock() }
        defer {
            let ms = Date().timeIntervalSince(begin) * 1000
            if onMain { stateLock.lock(); currentSpan = nil; stateLock.unlock() }
            // 100 ms on the main thread is already a dropped frame or two;
            // off it, only log the slow ones to keep the soak log readable.
            if onMain || ms > 100 {
                log(String(format: "%@ took %.0f ms%@", name, ms,
                           (onMain && ms > 100) ? "  <-- SLOW (main thread)" : ""))
            }
        }
        return try body()
    }

    private func startHeartbeat() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: .milliseconds(500))
        var ticks = 0
        t.setEventHandler { [weak self] in
            guard let self else { return }
            self.checkMainThread()
            ticks += 1
            if ticks % 20 == 0 { self.heartbeat() }   // every 10s
        }
        t.resume()
        heartbeatTimer = t
    }

    private func checkMainThread() {
        stateLock.lock()
        let stalled = Date().timeIntervalSince(lastMainPing)
        let span = currentSpan
        let alreadyReported = reportedStall
        if stalled > 2.0 { reportedStall = true }
        stateLock.unlock()

        guard stalled > 2.0 else { return }
        // Re-log every 2s while stuck: the last line before a 0x8badf00d kill
        // then shows how far it got. iOS kills at 10s.
        let what = span.map { "in \($0.name)" } ?? "(no span active)"
        if !alreadyReported || Int(stalled) % 2 == 0 {
            log(String(format: "MAIN THREAD STALLED %.1fs %@ — watchdog kills at 10s",
                       stalled, what))
        }
    }

    // MARK: - Stats

    /// Cheap snapshot of what jetsam actually measures.
    static func memoryFootprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size
                                           / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard kr == KERN_SUCCESS else { return 0 }
        return Double(info.phys_footprint) / (1024 * 1024)
    }

    var uptime: TimeInterval { Date().timeIntervalSince(started) }

    static func hms(_ seconds: TimeInterval) -> String {
        let s = Int(seconds)
        return String(format: "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60)
    }

    /// Set by the app model each refresh so the heartbeat can print engine
    /// state without calling into the engine from this queue.
    private var snapshot = "engine idle"
    func setSnapshot(_ text: String) { queue.async { self.snapshot = text } }
    func countClipStart(_ name: String) {
        clipStarts += 1
        log("clip #\(clipStarts): \(name)")
    }

    private func heartbeat() {
        let thermal: String
        switch ProcessInfo.processInfo.thermalState {
        case .nominal:  thermal = "nominal"
        case .fair:     thermal = "fair"
        case .serious:  thermal = "SERIOUS"
        case .critical: thermal = "CRITICAL"
        @unknown default: thermal = "?"
        }
        log(String(format: "hb up=%@ mem=%.0fMB thermal=%@ clips=%d | %@",
                   Self.hms(uptime), Self.memoryFootprintMB(), thermal,
                   clipStarts, snapshot))
    }

    // MARK: - Writing

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    func log(_ message: String, source: String = "app") {
        let line = "\(Self.stamp.string(from: Date())) [\(source)] \(message)\n"
        queue.async { self.write(line) }
    }

    private func write(_ line: String) {
        guard fd >= 0, let data = line.data(using: .utf8) else { return }
        data.withUnsafeBytes { _ = Darwin.write(fd, $0.baseAddress!, data.count) }
        written += UInt64(data.count)
        if written > Self.MAX_BYTES {
            close(fd)
            rotate(at: Self.logURL)
            fd = open(Self.logURL.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
            gCrashFD = fd
            written = 0
        }
    }
}
