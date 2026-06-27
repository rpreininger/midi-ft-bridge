// ====================================================================
//  ConfigEditorView - In-app editor for the whole config, replacing
//  hand-editing config.json. Edits a working copy; the parent writes it
//  back to disk on Save.
// ====================================================================
import SwiftUI

struct ConfigEditorView: View {
    @Binding var config: AppConfig
    let savePath: String          // shown in the footer; "" = will prompt
    let isEngineRunning: Bool
    let onSave: () -> Void
    let onCancel: () -> Void

    private enum Tab: Hashable { case general, panels, mappings }
    @State private var tab: Tab = .general
    @State private var mappingsDropTargeted = false
    @State private var midiDevices: [String] = []

    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $tab) {
                Text("General").tag(Tab.general)
                Text("Panels (\(config.panels.count))").tag(Tab.panels)
                Text("Mappings (\(config.mappings.count))").tag(Tab.mappings)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding()

            Divider()

            switch tab {
            case .general:  generalTab
            case .panels:   panelsTab
            case .mappings: mappingsTab
            }

            Divider()
            footer
        }
        .frame(width: 660, height: 560)
        .onAppear { midiDevices = MFBEngine.availableMIDIDevices() }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack {
            if isEngineRunning {
                Label("Engine running — Stop & Start to apply changes",
                      systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.orange)
            } else if !savePath.isEmpty {
                Text(savePath)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Button("Cancel", action: onCancel)
                .keyboardShortcut(.cancelAction)
            Button("Save", action: onSave)
                .keyboardShortcut(.defaultAction)
                .disabled(config.panels.isEmpty)
        }
        .padding()
    }

    // MARK: - General

    private var generalTab: some View {
        Form {
            Section("Canvas") {
                labeledInt("Video width", $config.videoWidth)
                labeledInt("Video height", $config.videoHeight)
                labeledInt("Default FPS", $config.defaultFps)
            }
            Section("Paths & Audio") {
                LabeledContent("Clips directory") {
                    TextField("", text: $config.clipsDir).multilineTextAlignment(.trailing)
                }
                LabeledContent("Audio device") {
                    TextField("empty = disabled", text: $config.audioDevice)
                        .multilineTextAlignment(.trailing)
                }
                .help("CoreAudio output device name, or empty to disable")
            }
            Section("MIDI & Network") {
                LabeledContent("MIDI input device") {
                    HStack(spacing: 6) {
                        Picker("", selection: $config.midiDevice) {
                            Text("All sources").tag("")
                            ForEach(midiDevices, id: \.self) { Text($0).tag($0) }
                            // Preserve a configured device that isn't currently plugged in.
                            if !config.midiDevice.isEmpty, !midiDevices.contains(config.midiDevice) {
                                Text("\(config.midiDevice) (not connected)").tag(config.midiDevice)
                            }
                        }
                        .labelsHidden()
                        Button {
                            midiDevices = MFBEngine.availableMIDIDevices()
                        } label: { Image(systemName: "arrow.clockwise") }
                            .buttonStyle(.borderless)
                            .help("Rescan MIDI devices")
                    }
                }
                .help("Which MIDI input to listen on. “All sources” connects to every device.")
                labeledInt("MIDI channel (0–15, −1 = any)", $config.midiChannel)
                labeledInt("Web status port", $config.webPort)
                Toggle("Verbose debug logging", isOn: Binding(
                    get: { config.debug != 0 },
                    set: { config.debug = $0 ? 1 : 0 }
                ))
            }
        }
        .formStyle(.grouped)
    }

    private func labeledInt(_ title: String, _ value: Binding<Int>) -> some View {
        LabeledContent(title) {
            TextField("", value: value, format: .number)
                .multilineTextAlignment(.trailing)
                .frame(width: 90)
        }
    }

    // MARK: - Panels

    private var panelsTab: some View {
        VStack(spacing: 0) {
            toolbar {
                Button {
                    var p = ConfigPanel()
                    p.name = "panel\(config.panels.count + 1)"
                    config.panels.append(p)
                } label: { Label("Add Panel", systemImage: "plus") }
            }
            ScrollView {
                VStack(spacing: 12) {
                    ForEach($config.panels) { $panel in
                        panelCard($panel)
                    }
                    if config.panels.isEmpty {
                        emptyHint("No panels. Add at least one output panel.")
                    }
                }
                .padding()
            }
        }
    }

    private func panelCard(_ panel: Binding<ConfigPanel>) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    TextField("Name", text: panel.name)
                        .frame(maxWidth: 180)
                    Picker("", selection: panel.type) {
                        Text("Flaschen-Taschen").tag("ft")
                        Text("BLE").tag("ble")
                        Text("BLE-UDP").tag("ble_udp")
                    }
                    .labelsHidden()
                    .frame(maxWidth: 160)
                    Spacer()
                    Button(role: .destructive) {
                        config.panels.removeAll { $0.id == panel.wrappedValue.id }
                    } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                }

                HStack {
                    TextField("IP", text: panel.ip).frame(maxWidth: 160)
                    intField("Port", panel.port, width: 90)
                    if panel.wrappedValue.maxFps > 0 || panel.wrappedValue.isBle {
                        intField("Max FPS", panel.maxFps, width: 90)
                    } else {
                        intField("Max FPS (0=off)", panel.maxFps, width: 130)
                    }
                }

                HStack {
                    Text("Source region").font(.caption).foregroundStyle(.secondary)
                    intField("x", panel.srcX, width: 70)
                    intField("y", panel.srcY, width: 70)
                    intField("w", panel.srcW, width: 70)
                    intField("h", panel.srcH, width: 70)
                }

                if panel.wrappedValue.isBle {
                    HStack {
                        TextField("BLE name", text: panel.bleName).frame(maxWidth: 160)
                        TextField("BLE addr", text: panel.bleAddr).frame(maxWidth: 200)
                        intField("Brightness", panel.brightness, width: 110)
                    }
                }
            }
            .padding(6)
        }
    }

    // MARK: - Mappings

    private var mappingsTab: some View {
        VStack(spacing: 0) {
            toolbar {
                Button {
                    var m = ConfigMapping()
                    m.note = (config.mappings.map(\.note).max() ?? 35) + 1
                    config.mappings.append(m)
                } label: { Label("Add Mapping", systemImage: "plus") }
            }
            ScrollView {
                VStack(spacing: 6) {
                    headerRow
                    ForEach($config.mappings) { $m in
                        mappingRow($m)
                    }
                    if config.mappings.isEmpty {
                        emptyHint("No mappings. Add a note → clip mapping, or drop clip files here.")
                    }
                }
                .padding()
                .frame(maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
                .contentShape(Rectangle())
            }
            // Drop clips onto empty space to create new mappings. (Dropping
            // onto a row's clip field instead just fills that row — the inner
            // destination wins when the cursor is over it.)
            .dropDestination(for: URL.self) { urls, _ in
                let added = addMappings(for: urls)
                return added > 0
            } isTargeted: { mappingsDropTargeted = $0 }
            .overlay {
                if mappingsDropTargeted {
                    RoundedRectangle(cornerRadius: 8)
                        .strokeBorder(Color.accentColor, lineWidth: 2)
                        .padding(4)
                        .allowsHitTesting(false)
                }
            }
        }
    }

    private var headerRow: some View {
        HStack {
            Text("Note").frame(width: 70, alignment: .leading)
            Text("Clip").frame(maxWidth: .infinity, alignment: .leading)
            Text("Panel").frame(width: 130, alignment: .leading)
            Spacer().frame(width: 24)
        }
        .font(.caption).foregroundStyle(.secondary)
        .padding(.horizontal, 4)
    }

    private func mappingRow(_ m: Binding<ConfigMapping>) -> some View {
        HStack {
            TextField("", value: m.note, format: .number)
                .frame(width: 70)
            TextField("clip filename", text: m.clip)
                .frame(maxWidth: .infinity)
                .dropDestination(for: URL.self) { urls, _ in
                    guard let url = urls.first(where: \.isFileURL) else { return false }
                    m.clip.wrappedValue = clipPath(for: url)
                    return true
                }
            Picker("", selection: m.panel) {
                ForEach(config.panelTargets, id: \.self) { Text($0).tag($0) }
                // Preserve a value that no longer matches a panel name.
                if !config.panelTargets.contains(m.wrappedValue.panel) {
                    Text(m.wrappedValue.panel).tag(m.wrappedValue.panel)
                }
            }
            .labelsHidden()
            .frame(width: 130)
            Button(role: .destructive) {
                config.mappings.removeAll { $0.id == m.wrappedValue.id }
            } label: { Image(systemName: "trash") }
                .buttonStyle(.borderless)
        }
    }

    // MARK: - Drag & drop

    /// Append one mapping per dropped file; returns how many were added.
    @discardableResult
    private func addMappings(for urls: [URL]) -> Int {
        let files = urls.filter(\.isFileURL)
        for url in files {
            var m = ConfigMapping()
            m.note = (config.mappings.map(\.note).max() ?? 35) + 1
            m.clip = clipPath(for: url)
            config.mappings.append(m)
        }
        return files.count
    }

    /// A dropped file is stored relative to `clips_dir` when it lives inside
    /// that directory (e.g. ".../clips/mp4/Axel.mp4" → "mp4/Axel.mp4"), so the
    /// value matches how clips are referenced elsewhere; otherwise we fall
    /// back to the bare filename.
    private func clipPath(for url: URL) -> String {
        let path = url.standardizedFileURL.path
        let dirName = (config.clipsDir as NSString).lastPathComponent
        if !dirName.isEmpty, dirName != "." {
            let marker = "/\(dirName)/"
            if let r = path.range(of: marker, options: .backwards) {
                return String(path[r.upperBound...])
            }
        }
        return url.lastPathComponent
    }

    // MARK: - Helpers

    @ViewBuilder
    private func toolbar<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        HStack {
            content()
            Spacer()
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
        .background(Color(NSColor.windowBackgroundColor))
    }

    private func intField(_ title: String, _ value: Binding<Int>, width: CGFloat? = nil) -> some View {
        let field = TextField(title, value: value, format: .number)
            .textFieldStyle(.roundedBorder)
        return Group {
            if let width { field.frame(width: width) } else { field }
        }
    }

    private func emptyHint(_ text: String) -> some View {
        Text(text)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 40)
    }
}
