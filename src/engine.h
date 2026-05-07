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
    void triggerMapping(int mappingIdx);

    // Trigger a clip by MIDI note (looks up mapping; no-op if not mapped).
    void triggerNote(int note);

    // Stop the currently playing clip and blank the panels.
    void stopActiveClip();

    // Toggle pause on the active clip; no-op if none.
    void togglePause();
    bool isClipPaused() const;

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

private:
    void workerLoop();
    void sendBlackToAll();
    void shutdownInternals();

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

    // --- Active clip (touched by both worker and triggerMapping callers) ---
    mutable std::mutex m_clipMutex;
    std::unique_ptr<ClipPlayer> m_activeClip;
    std::string m_activeClipName;

    // --- Per-panel scratch / throttle state (worker-only) ---
    std::vector<std::vector<uint8_t>> m_regionBuffers;
    std::vector<std::chrono::steady_clock::time_point> m_lastPanelSend;

    // --- Frame callback (set by UI thread, called from worker) ---
    mutable std::mutex m_callbackMutex;
    FrameCallback m_frameCallback;
    std::function<void()> m_shutdownCallback;

    std::thread m_worker;
    std::atomic<bool> m_running{false};
    int64_t m_startTime{0};
    bool m_started{false};
};
