// ====================================================================
//  Status Server - HTTP status/control interface for midi-ft-bridge
// ====================================================================
#pragma once

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "config.h"

struct PanelStatus {
    std::string name;
    std::string ip;
    int port;
    uint64_t framesSent;
    uint64_t bytesSent;
    bool enabled;
    std::string activeClip;  // Currently playing clip name, empty if idle
};

struct MidiEventLog {
    std::string type;   // "NOTE_ON" or "NOTE_OFF"
    int note;
    int velocity;
    int channel;
    uint64_t timestamp; // milliseconds since start
};

class StatusServer {
public:
    StatusServer(int port = 8080);
    ~StatusServer();

    bool start();
    void stop();

    // Update panel status (called from main loop)
    void updatePanelStatus(const std::vector<PanelStatus>& panels);

    // Log a MIDI event (called from main loop)
    void logMidiEvent(const MidiEventLog& event);

    // Update MIDI device name
    void setMidiDevice(const std::string& name);

    // Update uptime
    void setStartTime(uint64_t epochMs);

    // Set config reference for display
    void setConfig(const Config* config) { m_config = config; }

    // Set callback for triggering test clips
    void setTestCallback(std::function<void(int note)> cb) { m_testCallback = cb; }

private:
    void serverThread();
    void handleClient(int clientSocket);
    std::string generateHTML();
    std::string generateStatusJSON();

    int m_port;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_statusMutex;
    std::vector<PanelStatus> m_panelStatus;
    std::vector<MidiEventLog> m_midiLog;
    std::string m_midiDevice;
    uint64_t m_startTime = 0;
    const Config* m_config = nullptr;
    std::function<void(int note)> m_testCallback;

    static constexpr int MAX_MIDI_LOG = 50;
};
