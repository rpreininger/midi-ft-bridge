// ====================================================================
//  MIDI Input - ALSA sequencer MIDI listener
// ====================================================================
#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>

struct MidiEvent {
    enum Type { NOTE_ON, NOTE_OFF };
    Type type;
    int note;       // 0-127
    int velocity;   // 0-127
    int channel;    // 0-15
};

class MidiInput {
public:
    MidiInput();
    ~MidiInput();

    // Names of all MIDI input sources currently available on the system.
    // Static so the UI can populate a device picker without a running engine.
    static std::vector<std::string> availableDevices();

    // Restrict which source(s) start() connects to: a display-name substring
    // (case-insensitive). Empty (the default) connects to every source.
    // Must be called before start() to take effect.
    void setPreferredDevice(const std::string& name) { m_preferredDevice = name; }

    // Switch the preferred device while running: disconnects the currently
    // connected sources and connects those matching `name` instead, without
    // restarting the engine. Safe to call before start() too (behaves like
    // setPreferredDevice). Returns true if at least one source connected.
    bool switchDevice(const std::string& name);

    // Start the MIDI listener thread
    // Auto-discovers and connects to USB MIDI devices
    bool start();

    // Stop the listener
    void stop();

    // Poll for the next MIDI event (non-blocking)
    // Returns true if an event was available
    bool getNextEvent(MidiEvent& event);

    // Check if running
    bool isRunning() const { return m_running.load(); }

    // Get connected device name
    std::string getDeviceName() const;

    // Get event count for statistics
    uint64_t getEventCount() const { return m_eventCount.load(); }

    // Push an event onto the queue (thread-safe; called from platform listeners)
    void enqueueEvent(const MidiEvent& ev) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_eventQueue.push(ev);
        ++m_eventCount;
    }

private:
    void listenerThread();

    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    std::queue<MidiEvent> m_eventQueue;
    std::atomic<uint64_t> m_eventCount;
    // Device selection state. Guarded by m_deviceMutex because switchDevice()
    // and getDeviceName() are called from the UI thread while the CoreMIDI
    // callback thread is delivering packets.
    mutable std::mutex m_deviceMutex;
    std::string m_deviceName;
    std::string m_preferredDevice;   // empty = connect to all sources
    std::vector<uint32_t> m_connectedSources;  // MIDIEndpointRefs currently connected

    // Connect every source matching m_preferredDevice, filling m_deviceName and
    // m_connectedSources. Caller must hold m_deviceMutex.
    int connectMatchingSourcesLocked();

    // ALSA sequencer handle (void* to avoid header dependency)
    void* m_seqHandle;
    int m_seqPort;
};
