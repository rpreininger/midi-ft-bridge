// ====================================================================
//  MIDI Input - Stub implementation for macOS
//  No MIDI hardware support; use --test mode for keyboard input.
// ====================================================================

#include "midi_input.h"
#include <iostream>

MidiInput::MidiInput()
    : m_running(false)
    , m_eventCount(0)
    , m_seqHandle(nullptr)
    , m_seqPort(-1)
{
}

MidiInput::~MidiInput() {
    stop();
}

bool MidiInput::start() {
    if (m_running) return true;
    m_running = true;
    std::cerr << "MidiInput: Stub mode (no ALSA). Use --test for keyboard input." << std::endl;
    return true;
}

void MidiInput::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool MidiInput::getNextEvent(MidiEvent& event) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_eventQueue.empty()) return false;
    event = m_eventQueue.front();
    m_eventQueue.pop();
    return true;
}

std::string MidiInput::getDeviceName() const {
    return "Stub (no MIDI)";
}

void MidiInput::listenerThread() {
    // No-op in stub
}
