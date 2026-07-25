// ====================================================================
//  Engine - Headless orchestrator for midi-ft-bridge
//
//  Owns config, MIDI input, audio player, panel senders (FT/BLE), the
//  active ClipPlayer, and the optional HTTP status server. Runs the
//  main render loop on its own worker thread so callers (CLI or GUI)
//  can drive UI / signal handling on their own thread.
// ====================================================================
#pragma once

#include "config.h"
#include "midi_input.h"
#include "clip_player.h"
#include "audio_player.h"
#include "ft_sender.h"
#include "ble_sender.h"
#include "status_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Engine {
public:
    // Frame callback signature: full canvas RGB24 (config.video_width x
    // config.video_height) before per-panel extraction. Invoked from the
    // engine worker thread; keep it short — copy and dispatch.
    using FrameCallback = std::function<void(const uint8_t* rgb, int width, int height)>;

    Engine();
    ~Engine();

    // Load config and start the engine + worker thread.
    // If statusServerEnabled is true, also starts the HTTP server on config.web_port.
    bool start(const std::string& configPath, bool statusServerEnabled = true);

    // Signal the worker to stop, join it, and tear everything down.
    void stop();

    bool isRunning() const { return m_running.load(); }

    // Trigger a clip by mapping index (0..mappings.size()-1).
    // Returns true if a clip actually started playing, false if the index is
    // out of range or the clip file could not be opened.
    bool triggerMapping(int mappingIdx);

    // Trigger a clip by MIDI note (looks up mapping; no-op if not mapped).
    void triggerNote(int note);

    // Stop the currently playing clip and blank the panels.
    void stopActiveClip();

    // Toggle pause on the active clip; no-op if none.
    void togglePause();
    bool isClipPaused() const;

    // Auto-play / test mode: when enabled, each clip that finishes advances to
    // the next mapping in order, wrapping around endlessly. Enabling while idle
    // starts the loop immediately; disabling lets the current clip play out but
    // stops further advancing.
    void setAutoPlay(bool on);
    bool isAutoPlay() const { return m_autoPlay.load(); }

    // Transport: current clip position / duration in seconds (0 if idle).
    double getPosition() const;
    double getDuration() const;

    // Seek the active clip to an absolute position, or by a relative delta.
    void seekTo(double seconds);
    void seekBy(double deltaSeconds);

    // Jump to the next / previous mapping (clip) in the playlist, wrapping.
    void skipToNext();
    void skipToPrevious();

    // Active clip name (empty if none playing).
    std::string getActiveClipName() const;

    // Subscribe to full-canvas frames. Pass nullptr to clear.
    void setFrameCallback(FrameCallback cb);

    // Callback invoked when the engine wants the host process to terminate
    // (e.g. user clicked "Shutdown Hub" in the web UI). Set by the host.
    void setShutdownCallback(std::function<void()> cb);

    // Read-only snapshot for UI rendering. Stable after start() until stop().
    const Config& getConfig() const { return m_config; }

    std::string getMidiDeviceName() const;

    // Switch MIDI input source while running (display-name substring; empty =
    // all sources). Updates the in-memory config so a later config save keeps
    // the choice. Returns true if at least one source connected.
    bool setMidiDevice(const std::string& name);

    // Latest per-panel live status (frames/bytes sent, connected, active clip).
    // Refreshed each worker tick; safe to call from any thread.
    std::vector<PanelStatus> getPanelStatus() const;

    // SSH `sudo shutdown now` to FT panels. Returns a per-panel result summary.
    // shutdownPanels() targets every shutdownable FT panel; shutdownPanel()
    // targets one by name. Both no-op for ble/loopback panels.
    std::string shutdownPanels();
    std::string shutdownPanel(const std::string& name);

private:
    void workerLoop();
    void sendBlackToAll();
    void shutdownInternals();

    // Start mapping `idx`; if its clip fails to open, advance through the
    // following mappings (wrapping) until one plays. Returns false only if no
    // mapping in the list is playable. Keeps auto-play from stalling forever on
    // a missing/empty clip (e.g. an empty "clip" entry in the config).
    bool startPlayableFrom(int idx);

    static void extractRegion(const uint8_t* src, int srcWidth,
                              int sx, int sy, int sw, int sh,
                              uint8_t* dst);

    // --- Owned resources, set up in start() ---
    Config m_config;
    std::vector<std::unique_ptr<FTSender>>  m_senders;
    std::vector<std::unique_ptr<BleSender>> m_bleSenders;
    AudioPlayer m_audioPlayer;
    MidiInput   m_midiInput;
    std::unique_ptr<StatusServer> m_statusServer;
    std::map<int, int> m_noteMappings;  // MIDI note -> mapping index

    // --- Latest panel status snapshot (written by worker, read by UI) ---
    mutable std::mutex m_panelStatusMutex;
    std::vector<PanelStatus> m_lastPanelStatus;

    // --- Active clip (touched by both worker and triggerMapping callers) ---
    mutable std::mutex m_clipMutex;
    std::unique_ptr<ClipPlayer> m_activeClip;
    std::string m_activeClipName;

    // Worker-owned copy of the current canvas. getCurrentFrame() hands back a
    // pointer into the ClipPlayer's own buffer, which triggerMapping() is free
    // to destroy the moment m_clipMutex is released - so the worker copies the
    // frame out under the lock and works from the copy afterwards.
    std::vector<uint8_t> m_canvasBuffer;

    // --- Per-panel scratch / throttle state (worker-only) ---
    std::vector<std::vector<uint8_t>> m_regionBuffers;
    std::vector<std::chrono::steady_clock::time_point> m_lastPanelSend;

    // --- Frame callback (set by UI thread, called from worker) ---
    mutable std::mutex m_callbackMutex;
    FrameCallback m_frameCallback;
    std::function<void()> m_shutdownCallback;

    // --- Auto-play / test mode ---
    std::atomic<bool> m_autoPlay{false};
    std::atomic<int>  m_autoPlayIndex{-1};  // last-triggered mapping index

    std::thread m_worker;
    std::atomic<bool> m_running{false};
    int64_t m_startTime{0};
    bool m_started{false};
};
