// ====================================================================
//  MIDI Input - ALSA sequencer implementation
// ====================================================================

#include "midi_input.h"
#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>

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

    snd_seq_t* seq = nullptr;

    // Open ALSA sequencer
    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0);
    if (err < 0) {
        std::cerr << "MidiInput: Failed to open ALSA sequencer: "
                  << snd_strerror(err) << std::endl;
        return false;
    }

    snd_seq_set_client_name(seq, "midi-ft-bridge");

    // Create input port
    m_seqPort = snd_seq_create_simple_port(seq, "MIDI In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);

    if (m_seqPort < 0) {
        std::cerr << "MidiInput: Failed to create port: "
                  << snd_strerror(m_seqPort) << std::endl;
        snd_seq_close(seq);
        return false;
    }

    // Auto-connect to USB MIDI devices
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    int myClient = snd_seq_client_id(seq);
    bool connected = false;

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == myClient) continue;
        if (client == 0) continue; // Skip system client

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);

        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);

            // Look for readable MIDI output ports
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;

            int srcClient = snd_seq_port_info_get_client(pinfo);
            int srcPort = snd_seq_port_info_get_port(pinfo);

            err = snd_seq_connect_from(seq, m_seqPort, srcClient, srcPort);
            if (err >= 0) {
                m_deviceName = snd_seq_client_info_get_name(cinfo);
                std::cerr << "MidiInput: Connected to " << m_deviceName
                          << " (" << srcClient << ":" << srcPort << ")" << std::endl;
                connected = true;
            }
        }
    }

    if (!connected) {
        std::cerr << "MidiInput: No MIDI devices found, waiting for connections..." << std::endl;
    }

    m_seqHandle = seq;
    m_running = true;
    m_thread = std::thread(&MidiInput::listenerThread, this);

    return true;
}

void MidiInput::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_seqHandle) {
        snd_seq_close((snd_seq_t*)m_seqHandle);
        m_seqHandle = nullptr;
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
    return m_deviceName;
}

void MidiInput::listenerThread() {
    snd_seq_t* seq = (snd_seq_t*)m_seqHandle;

    // Set up polling with timeout so we can check m_running
    int npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    std::vector<struct pollfd> pfds(npfds);
    snd_seq_poll_descriptors(seq, pfds.data(), npfds, POLLIN);

    while (m_running) {
        int ret = poll(pfds.data(), npfds, 100); // 100ms timeout
        if (ret <= 0) continue;

        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            MidiEvent midiEvent;

            switch (ev->type) {
                case SND_SEQ_EVENT_NOTEON:
                    if (ev->data.note.velocity > 0) {
                        midiEvent.type = MidiEvent::NOTE_ON;
                    } else {
                        // Note On with velocity 0 = Note Off
                        midiEvent.type = MidiEvent::NOTE_OFF;
                    }
                    midiEvent.note = ev->data.note.note;
                    midiEvent.velocity = ev->data.note.velocity;
                    midiEvent.channel = ev->data.note.channel;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_eventQueue.push(midiEvent);
                    }
                    m_eventCount++;
                    break;

                case SND_SEQ_EVENT_NOTEOFF:
                    midiEvent.type = MidiEvent::NOTE_OFF;
                    midiEvent.note = ev->data.note.note;
                    midiEvent.velocity = ev->data.note.velocity;
                    midiEvent.channel = ev->data.note.channel;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_eventQueue.push(midiEvent);
                    }
                    m_eventCount++;
                    break;

                default:
                    break;
            }
        }
    }

    std::cerr << "MidiInput: Listener stopped" << std::endl;
}
