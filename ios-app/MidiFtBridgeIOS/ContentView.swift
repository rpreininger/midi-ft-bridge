// ====================================================================
//  ContentView - minimal operator surface for the iOS spike.
//
//  Not the eventual UI. Enough to start the engine, fire clips, and see
//  that frames and panel counters are moving.
// ====================================================================
import SwiftUI

/// What the top of the screen shows. Only ever one of the two: showing both
/// left the clip list a few rows tall on a phone, and during a set the
/// rundown is what the operator actually needs to see.
enum TopPane: String {
    case video, status
}

struct ContentView: View {
    @EnvironmentObject var model: IOSAppModel
    @State private var confirmShutdown = false

    /// Remembered across launches — an operator who works from the rundown
    /// should not have to re-pick it at every gig.
    @AppStorage("topPane") private var topPane: TopPane = .video

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                // Tap the panel itself to swap, as well as the toolbar button.
                topArea
                    .contentShape(Rectangle())
                    .onTapGesture { toggleTopPane() }
                stoppedHint
                Divider()
                clipList
                transport
            }
            .padding()
            .navigationTitle("MIDI-FT Bridge")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button(role: .destructive) {
                        confirmShutdown = true
                    } label: {
                        Image(systemName: "power")
                    }
                    .tint(.orange)
                }
                // In the toolbar rather than on screen: a switch that costs
                // vertical space would defeat the point of the switch.
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        toggleTopPane()
                    } label: {
                        Image(systemName: topPane == .video
                              ? "chart.bar.doc.horizontal" : "tv")
                    }
                    .accessibilityLabel(topPane == .video ? "Show status" : "Show video")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button(model.running ? "Stop" : "Start") {
                        model.running ? model.stop() : model.start()
                    }
                    .tint(model.running ? .red : .accentColor)
                    // Engine start/stop now runs off the main thread; block
                    // a second tap rather than queueing it behind the first.
                    .disabled(model.busy)
                }
            }
            .confirmationDialog("Shut down all panels?",
                                isPresented: $confirmShutdown, titleVisibility: .visible) {
                Button("Shut down panels", role: .destructive) { model.shutdownPanels() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("Powers off every FT panel. They must be switched back on by hand.")
            }
            .alert("Panel shutdown",
                   isPresented: Binding(get: { model.shutdownResult != nil },
                                        set: { if !$0 { model.shutdownResult = nil } })) {
                Button("OK") { model.shutdownResult = nil }
            } message: {
                Text(model.shutdownResult ?? "")
            }
        }
        .onAppear {
            model.refreshState()
            // Start on launch when there is a config to start from. Without
            // this the whole UI sits disabled behind an unexplained Start
            // button, which reads as "the app is broken".
            if !model.running && model.configExists {
                model.start()
            }
        }
    }

    private func toggleTopPane() {
        withAnimation(.easeInOut(duration: 0.15)) {
            topPane = (topPane == .video) ? .status : .video
        }
    }

    @ViewBuilder
    private var topArea: some View {
        switch topPane {
        case .video:  preview
        case .status: statusPane
        }
    }

    /// Shown in both panes, because "the engine is not running" is the one
    /// thing you must not be able to hide from yourself. One line, and only
    /// while stopped, so it costs nothing during a set.
    @ViewBuilder
    private var stoppedHint: some View {
        if !model.running {
            Text("Engine stopped — tap Start to trigger clips.")
                .font(.caption.monospaced())
                .foregroundStyle(.orange)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var preview: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 8).fill(.black)
            if let img = model.previewImage {
                // Panels are chunky RGB matrices; never smooth them.
                Image(uiImage: img)
                    .interpolation(.none)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                Text("no signal")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            // Panel health overlaid rather than stacked: it costs no vertical
            // space, and losing sight of a dead panel is exactly how a dark
            // panel goes unnoticed for a whole set.
            if model.running {
                VStack {
                    Spacer()
                    HStack(spacing: 8) {
                        ForEach(model.panels) { p in
                            HStack(spacing: 3) {
                                Circle()
                                    .fill(p.connected ? .green : .red)
                                    .frame(width: 6, height: 6)
                                Text(p.name.prefix(1))
                            }
                        }
                        Spacer()
                    }
                    .font(.caption2.monospaced())
                    .foregroundStyle(.white.opacity(0.85))
                    .padding(6)
                }
            }
        }
        .frame(maxHeight: 220)
    }

    /// Deliberately two lines. A seven-line status block only bought two rows
    /// of rundown over the video pane; compressed, switching roughly doubles
    /// the visible clip list, which is the point of having the switch.
    private var statusPane: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(model.running
                 ? "\(model.status) · MIDI \(model.midiDevice.isEmpty ? "—" : model.midiDevice) · \(model.audioRoute)"
                 : model.status)
                .lineLimit(1)
                .truncationMode(.middle)

            if model.running {
                HStack(spacing: 10) {
                    ForEach(model.panels) { p in
                        HStack(spacing: 3) {
                            Circle()
                                .fill(p.connected ? .green : .red)
                                .frame(width: 6, height: 6)
                            // A healthy panel needs only its frame count; a
                            // dead one needs the address you would go and
                            // check, so that is where the detail belongs.
                            Text(p.connected
                                 ? "\(p.name.prefix(3)) \(p.framesSent)"
                                 : "\(p.name.prefix(3)) \(p.ip)")
                        }
                    }
                    Spacer(minLength: 0)
                }
                .lineLimit(1)
            }
        }
        .font(.caption2.monospaced())
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var clipList: some View {
        List(model.mappings) { m in
            Button {
                model.trigger(index: m.id)
            } label: {
                HStack {
                    Text("\(m.note)")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .frame(width: 34, alignment: .leading)
                    Text(m.clip)
                    Spacer()
                    if m.clip == model.activeClip {
                        Image(systemName: "play.fill").foregroundStyle(.green)
                    }
                }
            }
            .disabled(!model.running)
        }
        .listStyle(.plain)
    }

    private var transport: some View {
        VStack(spacing: 8) {
            HStack(spacing: 16) {
                Button("Pause") { model.togglePause() }
                Button("Stop clip") { model.stopClip() }
                // Soak test: same endless auto-play the Mac app has. Left
                // running for hours it exercises the clip-switch path, and
                // the log records memory, stalls and every clip start.
                Button(model.autoPlay ? "Stop Test Loop" : "Loop All (Test)") {
                    model.toggleLoop()
                }
                .tint(model.autoPlay ? .red : .accentColor)
            }
            .buttonStyle(.bordered)
            .disabled(!model.running)

            HStack(spacing: 12) {
                if !model.soakSummary.isEmpty {
                    Text(model.soakSummary)
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                }
                Spacer()
                // The log lives in Documents (visible in the Files app), but
                // at a venue AirDropping it to the Mac is far quicker.
                if FileManager.default.fileExists(atPath: Diagnostics.logURL.path) {
                    ShareLink(item: Diagnostics.logURL) {
                        Label("Log", systemImage: "square.and.arrow.up")
                            .font(.caption2)
                    }
                }
            }
        }
    }
}
