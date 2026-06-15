import SwiftUI

/// Info / help panel shown from the toolbar "?" button.
struct HelpView: View {
    var onClose: () -> Void

    private var version: String {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "—"
        let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? ""
        return b.isEmpty ? v : "\(v) (\(b))"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            HStack(spacing: 12) {
                Image(systemName: "pianokeys")
                    .font(.largeTitle)
                    .foregroundStyle(.tint)
                VStack(alignment: .leading, spacing: 2) {
                    Text("MIDI-FT Bridge").font(.title2).bold()
                    Text("Version \(version)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding()

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    section("About") {
                        Text("Drives Roland Fantom MIDI notes to LED panels (FT/UDP and Bluetooth). "
                           + "Native macOS build: video decode via AVFoundation (AVAssetReader) and "
                           + "audio via AudioToolbox (AudioQueue) as the master clock — no FFmpeg, "
                           + "SDL2 or Homebrew. Runs on macOS 13 and newer.")
                            .fixedSize(horizontal: false, vertical: true)
                    }

                    section("Keyboard shortcuts") {
                        shortcut("1 – 9, 0, -", "Trigger mapping 1–11")
                        shortcut("↑ / ↓", "Select previous / next mapping")
                        shortcut("Return", "Trigger the selected mapping")
                        shortcut("Space", "Pause / resume the current clip")
                        shortcut("Esc", "Stop the current clip")
                        Text("Shortcuts are ignored while typing in a text field.")
                            .font(.caption).foregroundStyle(.secondary)
                    }

                    section("Getting started") {
                        step(1, "Pick a config file (Browse…), or edit it with Edit Config…")
                        step(2, "Press Start to launch the engine.")
                        step(3, "Trigger clips from your MIDI keyboard, the number keys, or by "
                              + "double-clicking a mapping in the list.")
                        step(4, "Use the transport controls or scrubber to seek, pause and skip.")
                    }
                }
                .padding()
            }

            Divider()
            HStack {
                Link("Project on GitHub",
                     destination: URL(string: "https://github.com/rpreininger/midi-ft-bridge")!)
                    .font(.caption)
                Spacer()
                Button("Close") { onClose() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .frame(width: 460, height: 540)
    }

    // MARK: - building blocks

    @ViewBuilder
    private func section<Content: View>(_ title: String,
                                        @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            content()
        }
    }

    private func shortcut(_ keys: String, _ desc: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(keys)
                .font(.system(.body, design: .monospaced))
                .frame(width: 110, alignment: .leading)
            Text(desc)
            Spacer()
        }
    }

    private func step(_ n: Int, _ desc: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("\(n).").bold().frame(width: 18, alignment: .trailing)
            Text(desc).fixedSize(horizontal: false, vertical: true)
            Spacer()
        }
    }
}
