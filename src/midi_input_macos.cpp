// ====================================================================
//  MIDI Input - CoreMIDI implementation for macOS
//  Connects to all available MIDI sources at start; CoreMIDI delivers
//  packets on its own high-priority thread, which we parse and push
//  into the shared event queue.
// ====================================================================

#include "midi_input.h"

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <iostream>
#include <vector>

namespace {

std::string cfStringToStd(CFStringRef cfStr) {
    if (!cfStr) return {};
    CFIndex len = CFStringGetLength(cfStr);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(maxBytes));
    if (CFStringGetCString(cfStr, buf.data(), maxBytes, kCFStringEncodingUTF8)) {
        return std::string(buf.data());
    }
    return {};
}

std::string getEndpointName(MIDIEndpointRef endpoint) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr && name) {
        std::string s = cfStringToStd(name);
        CFRelease(name);
        if (!s.empty()) return s;
    }
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name) == noErr && name) {
        std::string s = cfStringToStd(name);
        CFRelease(name);
        return s;
    }
    return "Unknown";
}

// Called by CoreMIDI on its own thread for every incoming packet list.
void midiReadProc(const MIDIPacketList* pktList, void* readProcRefCon, void* /*srcConnRefCon*/) {
    MidiInput* self = static_cast<MidiInput*>(readProcRefCon);
    const MIDIPacket* pkt = &pktList->packet[0];

    for (UInt32 i = 0; i < pktList->numPackets; ++i) {
        const Byte* data = pkt->data;
        UInt16 len = pkt->length;
        UInt16 pos = 0;

        while (pos < len) {
            Byte status = data[pos];
            // Skip stray data bytes (running status not used over USB MIDI)
            if ((status & 0x80) == 0) { ++pos; continue; }

            Byte type    = status & 0xF0;
            Byte channel = status & 0x0F;

            if (type == 0x90 || type == 0x80) {       // Note On / Note Off (3 bytes)
                if (pos + 2 >= len) break;
                int note     = data[pos + 1];
                int velocity = data[pos + 2];

                MidiEvent ev;
                ev.type = (type == 0x90 && velocity > 0)
                              ? MidiEvent::NOTE_ON
                              : MidiEvent::NOTE_OFF;
                ev.note     = note;
                ev.velocity = velocity;
                ev.channel  = channel;
                self->enqueueEvent(ev);

                pos += 3;
            } else if (type == 0xC0 || type == 0xD0) { // PC / channel pressure (2 bytes)
                pos += 2;
            } else if (type == 0xF0) {                 // SysEx / system: skip rest of packet
                break;
            } else {                                   // CC, pitch bend, aftertouch (3 bytes)
                pos += 3;
            }
        }

        pkt = MIDIPacketNext(pkt);
    }
}

} // namespace

MidiInput::MidiInput()
    : m_running(false)
    , m_eventCount(0)
    , m_seqHandle(nullptr)
    , m_seqPort(0)
{
}

MidiInput::~MidiInput() {
    stop();
}

bool MidiInput::start() {
    if (m_running) return true;

    MIDIClientRef client = 0;
    OSStatus err = MIDIClientCreate(CFSTR("midi-ft-bridge"), nullptr, nullptr, &client);
    if (err != noErr) {
        std::cerr << "MidiInput: MIDIClientCreate failed (" << err << ")" << std::endl;
        return false;
    }

    MIDIPortRef port = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    err = MIDIInputPortCreate(client, CFSTR("Input"), midiReadProc, this, &port);
#pragma clang diagnostic pop
    if (err != noErr) {
        std::cerr << "MidiInput: MIDIInputPortCreate failed (" << err << ")" << std::endl;
        MIDIClientDispose(client);
        return false;
    }

    ItemCount nSources = MIDIGetNumberOfSources();
    int connected = 0;
    for (ItemCount i = 0; i < nSources; ++i) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (!src) continue;

        std::string name = getEndpointName(src);
        OSStatus cerr = MIDIPortConnectSource(port, src, nullptr);
        if (cerr == noErr) {
            std::cerr << "MidiInput: Connected to " << name << std::endl;
            if (m_deviceName.empty()) m_deviceName = name;
            ++connected;
        } else {
            std::cerr << "MidiInput: Failed to connect to " << name
                      << " (" << cerr << ")" << std::endl;
        }
    }

    if (connected == 0) {
        std::cerr << "MidiInput: No MIDI sources found." << std::endl;
        m_deviceName = "(no MIDI sources)";
    }

    // Stash CoreMIDI handles in the existing void*/int slots
    m_seqHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(client));
    m_seqPort   = static_cast<int>(port);
    m_running   = true;
    return true;
}

void MidiInput::stop() {
    if (!m_running) return;
    m_running = false;

    auto port   = static_cast<MIDIPortRef>(m_seqPort);
    auto client = static_cast<MIDIClientRef>(reinterpret_cast<uintptr_t>(m_seqHandle));

    if (port)   { MIDIPortDispose(port);   m_seqPort   = 0; }
    if (client) { MIDIClientDispose(client); m_seqHandle = nullptr; }
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
    // Unused: CoreMIDI delivers callbacks on its own internal thread.
}
