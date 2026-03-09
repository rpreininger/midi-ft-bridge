// ====================================================================
//  MIDI Input - ALSA sequencer MIDI listener
// ====================================================================
#pragma once

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

private:
    void listenerThread();

    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    std::queue<MidiEvent> m_eventQueue;
    std::atomic<uint64_t> m_eventCount;
    std::string m_deviceName;

    // ALSA sequencer handle (void* to avoid header dependency)
    void* m_seqHandle;
    int m_seqPort;
};
