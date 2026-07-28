// ====================================================================
//  ContentView - minimal operator surface for the iOS spike.
//
//  Not the eventual UI. Enough to start the engine, fire clips, and see
//  that frames and panel counters are moving.
// ====================================================================
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: IOSAppModel
    @State private var confirmShutdown = false

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                preview
                statusLine
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
                ToolbarItem(placement: .topBarTrailing) {
                    Button(model.running ? "Stop" : "Start") {
                        model.running ? model.stop() : model.start()
                    }
                    .tint(model.running ? .red : .accentColor)
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
        }
        .frame(maxHeight: 220)
    }

    private var statusLine: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(model.status).font(.callout)
            if !model.running {
                // Every clip row is disabled while stopped; say so rather than
                // leaving the user tapping dead rows.
                Text("Engine stopped — tap Start to trigger clips.")
                    .foregroundStyle(.orange)
            }
            if model.running {
                Text("MIDI: \(model.midiDevice.isEmpty ? "—" : model.midiDevice)")
                Text("Audio route: \(model.audioRoute)")
                ForEach(model.panels) { p in
                    HStack(spacing: 6) {
                        Circle()
                            .fill(p.connected ? .green : .red)
                            .frame(width: 7, height: 7)
                        Text("\(p.name) \(p.ip) — \(p.framesSent) frames")
                    }
                }
            }
        }
        .font(.caption.monospaced())
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
        HStack(spacing: 16) {
            Button("Pause") { model.togglePause() }
            Button("Stop clip") { model.stopClip() }
        }
        .buttonStyle(.bordered)
        .disabled(!model.running)
    }
}
