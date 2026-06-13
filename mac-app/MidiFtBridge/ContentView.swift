import SwiftUI

/// What a pending shutdown confirmation targets.
private enum ShutdownTarget: Equatable {
    case all
    case panel(String)
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var keyMonitor: Any?
    @State private var shutdownTarget: ShutdownTarget?
    @State private var scrubValue = 0.0

    var body: some View {
        VSplitView {
            previewPane
                .frame(minHeight: 240)

            controlsPane
                .frame(minHeight: 220)
        }
        .onAppear { installKeyMonitor() }
        .onDisappear { removeKeyMonitor() }
        .confirmationDialog(
            shutdownPrompt,
            isPresented: shutdownDialogBinding,
            titleVisibility: .visible
        ) {
            Button("Shut Down", role: .destructive) { performShutdown() }
            Button("Cancel", role: .cancel) { shutdownTarget = nil }
        } message: {
            Text("This sends `sudo shutdown now` over SSH. Panels must be powered back on manually.")
        }
        .sheet(isPresented: $model.showConfigEditor) {
            ConfigEditorView(
                config: $model.editingConfig,
                savePath: model.configPath,
                isEngineRunning: model.running,
                onSave: { model.saveConfigEditor() },
                onCancel: { model.cancelConfigEditor() }
            )
        }
    }

    private func installKeyMonitor() {
        guard keyMonitor == nil else { return }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            // Don't intercept keys typed into text fields.
            if let resp = event.window?.firstResponder,
               resp.isKind(of: NSTextView.self) || resp is NSTextField {
                return event
            }

            // Arrow / Enter handling — allow auto-repeat for arrows so
            // holding navigates the list naturally.
            if let special = event.specialKey {
                switch special {
                case .upArrow:    model.selectPrevious(); return nil
                case .downArrow:  model.selectNext();     return nil
                case .enter, .carriageReturn:
                    model.triggerSelected()
                    return nil
                default: break
                }
            }

            if event.isARepeat { return event }
            if let chars = event.charactersIgnoringModifiers,
               model.handleKey(chars) {
                return nil
            }
            return event
        }
    }

    private func removeKeyMonitor() {
        if let m = keyMonitor {
            NSEvent.removeMonitor(m)
            keyMonitor = nil
        }
    }

    // MARK: - Preview

    private var previewPane: some View {
        ZStack {
            Color.black

            if !model.panels.isEmpty {
                GeometryReader { geo in
                    let scaled = fitSize(content: model.canvasSize, into: geo.size)
                    let offset = CGSize(
                        width: (geo.size.width - scaled.width) / 2,
                        height: (geo.size.height - scaled.height) / 2
                    )
                    let scale = scaled.width / max(model.canvasSize.width, 1)

                    // Faint outline of the full canvas so unused regions are visible.
                    Rectangle()
                        .stroke(.gray.opacity(0.3), lineWidth: 1)
                        .frame(width: scaled.width, height: scaled.height)
                        .offset(x: offset.width, y: offset.height)

                    // One tile per panel, positioned by src_x/src_y, sized by src_w/src_h.
                    ForEach(model.panels) { panel in
                        panelTile(panel: panel, scale: scale, baseOffset: offset)
                    }
                }
            } else {
                Text(model.running ? "loading config…" : "Engine stopped")
                    .foregroundStyle(.gray)
            }
        }
    }

    @ViewBuilder
    private func mappingRow(_ m: MappingInfo) -> some View {
        let isSelected = (model.selectedIndex == m.index)
        HStack {
            Text(AppModel.keyHint(for: m.index))
                .font(.system(.body, design: .monospaced).bold())
                .frame(width: 18, alignment: .center)
                .foregroundStyle(AppModel.keyHint(for: m.index).isEmpty
                                 ? .secondary : .primary)
            Text("Note \(m.note)")
                .font(.system(.body, design: .monospaced))
                .frame(width: 76, alignment: .leading)
            Text(m.clip)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            Text(m.panel)
                .font(.caption)
                .foregroundStyle(.secondary)
            Button("Trigger") { model.triggerMapping(m.index) }
                .buttonStyle(.borderless)
                .disabled(!model.running)
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 6)
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(isSelected
                      ? Color.accentColor.opacity(0.25)
                      : Color.clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 4)
                .stroke(isSelected ? Color.accentColor : Color.clear,
                        lineWidth: 1)
        )
        .contentShape(Rectangle())
        .onTapGesture(count: 2) {
            model.selectedIndex = m.index
            if model.running { model.triggerMapping(m.index) }
        }
        .onTapGesture(count: 1) {
            model.selectedIndex = m.index
        }
    }

    @ViewBuilder
    private func panelTile(panel: PanelInfo, scale: CGFloat, baseOffset: CGSize) -> some View {
        let w = CGFloat(panel.width) * scale
        let h = CGFloat(panel.height) * scale
        let x = baseOffset.width + CGFloat(panel.x) * scale
        let y = baseOffset.height + CGFloat(panel.y) * scale
        let img = model.panelImages[panel.name]

        ZStack(alignment: .topLeading) {
            if let img = img {
                Image(nsImage: img)
                    .resizable()
                    .interpolation(.none)
                    .frame(width: w, height: h)
            } else {
                Rectangle().fill(Color.black).frame(width: w, height: h)
            }
            Rectangle()
                .stroke(panelColor(for: panel.type), lineWidth: 1)
                .frame(width: w, height: h)
            Text(panel.name)
                .font(.system(size: 9, weight: .semibold))
                .padding(.horizontal, 3)
                .padding(.vertical, 1)
                .background(panelColor(for: panel.type).opacity(0.75))
                .foregroundStyle(.white)
                .padding(2)
        }
        .frame(width: w, height: h, alignment: .topLeading)
        .offset(x: x, y: y)
    }

    private func panelColor(for type: String) -> Color {
        switch type {
        case "ble":     return .pink
        case "ble_udp": return .purple
        default:        return .cyan
        }
    }

    private func fitSize(content: CGSize, into bounds: CGSize) -> CGSize {
        guard content.width > 0, content.height > 0 else { return .zero }
        let scale = min(bounds.width / content.width, bounds.height / content.height)
        return CGSize(width: content.width * scale, height: content.height * scale)
    }

    // MARK: - Controls

    private var controlsPane: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                TextField("Config path", text: $model.configPath)
                    .textFieldStyle(.roundedBorder)
                    .disabled(model.running)
                    .onSubmit {
                        if !model.running { model.start() }
                    }

                Button("Browse…") { model.chooseConfigFile() }
                    .disabled(model.running)

                Button("Edit Config…") { model.openConfigEditor() }

                if model.running {
                    Button("Stop") { model.stop() }
                } else {
                    Button("Start") { model.start() }
                        .keyboardShortcut(.return)
                }
            }

            HStack(spacing: 16) {
                Label(model.midiDeviceName.isEmpty ? "—" : model.midiDeviceName,
                      systemImage: "pianokeys")
                Label(model.activeClipName.isEmpty ? "idle" : model.activeClipName,
                      systemImage: model.clipPaused ? "pause.rectangle" : "play.rectangle")
                Spacer()
                if let err = model.lastError {
                    Text(err).foregroundStyle(.red).font(.caption)
                }
            }
            .foregroundStyle(.secondary)
            .font(.caption)

            Divider()

            HStack(spacing: 10) {
                Button("Stop Clip") { model.stopClip() }
                    .disabled(!model.running || model.activeClipName.isEmpty)
                Button(model.clipPaused ? "Resume" : "Pause") { model.togglePause() }
                    .disabled(!model.running || model.activeClipName.isEmpty)
                Button(model.autoPlay ? "Stop Test Loop" : "Loop All (Test)") {
                    model.toggleAutoPlay()
                }
                .tint(model.autoPlay ? .orange : nil)
                .disabled(!model.running || model.mappings.isEmpty)
                Spacer()
                Text("\(model.mappings.count) mappings")
                    .foregroundStyle(.secondary)
                    .font(.caption)
            }

            if model.running && !model.activeClipName.isEmpty {
                transportSection
            }

            if model.running && !model.panelStatus.isEmpty {
                panelStatusSection
                Divider()
            }

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(model.mappings) { m in
                            mappingRow(m)
                                .id(m.index)
                        }
                    }
                }
                .onChange(of: model.selectedIndex) { newIdx in
                    if let i = newIdx {
                        withAnimation(.easeOut(duration: 0.15)) {
                            proxy.scrollTo(i, anchor: .center)
                        }
                    }
                }
            }
            .background(Color(NSColor.controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .padding()
    }

    // MARK: - Transport

    private var transportSection: some View {
        VStack(spacing: 6) {
            HStack(spacing: 18) {
                Button { model.previousClip() } label: { Image(systemName: "backward.end.fill") }
                Button { model.seekBy(-10) }    label: { Image(systemName: "gobackward.10") }
                Button { model.togglePause() }  label: {
                    Image(systemName: model.clipPaused ? "play.fill" : "pause.fill")
                }
                Button { model.seekBy(10) }     label: { Image(systemName: "goforward.10") }
                Button { model.nextClip() }     label: { Image(systemName: "forward.end.fill") }
            }
            .buttonStyle(.borderless)
            .font(.title3)

            HStack(spacing: 8) {
                Text(timeString(model.position))
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                Slider(value: scrubBinding, in: 0...max(model.duration, 0.1)) { editing in
                    if editing {
                        scrubValue = model.position
                        model.isScrubbing = true
                    } else {
                        model.seek(to: scrubValue)
                        model.isScrubbing = false
                    }
                }
                .disabled(model.duration <= 0)
                Text(timeString(model.duration))
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
        }
    }

    private var scrubBinding: Binding<Double> {
        Binding(get: { model.isScrubbing ? scrubValue : model.position },
                set: { scrubValue = $0 })
    }

    private func timeString(_ s: Double) -> String {
        guard s.isFinite, s > 0 else { return "0:00" }
        let t = Int(s.rounded())
        return String(format: "%d:%02d", t / 60, t % 60)
    }

    // MARK: - Panel status

    private var canShutdownAny: Bool {
        model.panelStatus.contains { $0.canShutdown }
    }

    private var panelStatusSection: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text("Panels")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Shut Down All Panels") { shutdownTarget = .all }
                    .buttonStyle(.borderless)
                    .controlSize(.small)
                    .disabled(!canShutdownAny)
            }
            ForEach(model.panelStatus) { panelStatusRow($0) }
        }
    }

    @ViewBuilder
    private func panelStatusRow(_ p: PanelStatusInfo) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(statusColor(p))
                .frame(width: 9, height: 9)
            Text(p.name)
                .font(.system(.body, design: .monospaced))
                .frame(width: 70, alignment: .leading)
            Text(p.ip.isEmpty ? "—" : "\(p.ip):\(p.port)")
                .font(.caption).foregroundStyle(.secondary)
            Spacer()
            Text("\(p.framesSent) f · \(formatBytes(p.bytesSent))")
                .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            Button("Shut Down") { shutdownTarget = .panel(p.name) }
                .buttonStyle(.borderless)
                .controlSize(.small)
                .disabled(!p.canShutdown)
        }
        .padding(.vertical, 2)
        .padding(.horizontal, 6)
    }

    /// Red = not connected, green = playing a clip, gray = connected & idle.
    private func statusColor(_ p: PanelStatusInfo) -> Color {
        if !p.connected { return .red }
        return p.isPlaying ? .green : .gray
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        let b = Double(bytes)
        if b >= 1_073_741_824 { return String(format: "%.1f GB", b / 1_073_741_824) }
        if b >= 1_048_576     { return String(format: "%.1f MB", b / 1_048_576) }
        if b >= 1_024         { return String(format: "%.1f KB", b / 1_024) }
        return "\(bytes) B"
    }

    // MARK: - Shutdown confirmation

    private var shutdownDialogBinding: Binding<Bool> {
        Binding(get: { shutdownTarget != nil },
                set: { if !$0 { shutdownTarget = nil } })
    }

    private var shutdownPrompt: String {
        switch shutdownTarget {
        case .panel(let name): return "Shut down panel “\(name)”?"
        case .all:             return "Shut down all FT panels?"
        case .none:            return ""
        }
    }

    private func performShutdown() {
        switch shutdownTarget {
        case .panel(let name): model.shutdownPanel(name)
        case .all:             model.shutdownAllPanels()
        case .none:            break
        }
        shutdownTarget = nil
    }
}

#Preview {
    ContentView().environmentObject(AppModel())
}
