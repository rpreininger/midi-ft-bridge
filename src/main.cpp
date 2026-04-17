// ====================================================================
//  midi-ft-bridge
//  Receives USB MIDI events from Roland Fantom 06 and streams
//  mapped MP4 video clips to Flaschen-Taschen LED panels over WiFi
//
//  Audio-master architecture: audio playback drives the master clock,
//  video frames are picked to match. Falls back to wall-clock for
//  clips without audio.
// ====================================================================

#include "config.h"
#include "midi_input.h"
#include "clip_player.h"
#include "audio_player.h"
#include "ft_sender.h"
#include "ble_sender.h"
#include "status_server.h"

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <csignal>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

static std::atomic<bool> g_running{true};

static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// Extract a sub-rectangle from an RGB24 frame into a contiguous buffer
static void extractRegion(const uint8_t* src, int srcWidth,
                          int sx, int sy, int sw, int sh,
                          uint8_t* dst) {
    for (int row = 0; row < sh; row++) {
        memcpy(dst + row * sw * 3,
               src + (sy + row) * srcWidth * 3 + sx * 3,
               sw * 3);
    }
}

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --config <path>   Config file (default: config.json)\n"
              << "  --test            Keyboard test mode (F1-F12 trigger clips)\n"
              << "  --help            Show this help\n";
}

// Keyboard test mode: read F-keys from terminal in raw mode
// Returns the mapping index (0-11 for F1-F12), -1 if no key, -2 for ESC (stop)
static int pollKeyboard() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0}; // non-blocking

    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return -1;

    char buf[8] = {0};
    int n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n < 1) return -1;

    if (n == 1) {
        if (buf[0] >= '1' && buf[0] <= '9') return buf[0] - '1';
        if (buf[0] == '0') return 9;
        if (buf[0] == '-') return 10;
        if (buf[0] == '=') return 11;
        if (buf[0] == '\x1b') return -2;
        if (buf[0] == 'q' || buf[0] == 'Q') { g_running = false; return -1; }
        return -1;
    }

    if (buf[0] == '\x1b') {
        if (n >= 3 && buf[1] == 'O') {
            if (buf[2] == 'P') return 0;
            if (buf[2] == 'Q') return 1;
            if (buf[2] == 'R') return 2;
            if (buf[2] == 'S') return 3;
        }
        if (n >= 4 && buf[1] == '[' && buf[n-1] == '~') {
            int code = atoi(buf + 2);
            switch (code) {
                case 15: return 4;
                case 17: return 5;
                case 18: return 6;
                case 19: return 7;
                case 20: return 8;
                case 21: return 9;
                case 23: return 10;
                case 24: return 11;
            }
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    std::string configPath = "config.json";
    bool testMode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (strcmp(argv[i], "--test") == 0) {
            testMode = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Load config
    Config config;
    if (!config.load(configPath)) {
        std::cerr << "Failed to load config from " << configPath << std::endl;
        return 1;
    }

    // Initialize senders (FT for network panels, BLE for Bluetooth panels)
    std::vector<std::unique_ptr<FTSender>> senders;
    std::vector<std::unique_ptr<BleSender>> bleSenders;
    for (const auto& panel : config.panels) {
        if (panel.type == "ble") {
            auto ble = std::make_unique<BleSender>();
            ble->init(panel.ble_addr, panel.brightness, config.debug);
            senders.push_back(nullptr);
            bleSenders.push_back(std::move(ble));
        } else if (panel.type == "ble_udp") {
            auto sender = std::make_unique<FTSender>();
            if (!sender->init(panel.ip, panel.port)) {
                std::cerr << "Warning: Failed to init ble_udp sender for panel "
                          << panel.name << " (" << panel.ip << ":" << panel.port << ")" << std::endl;
            }
            senders.push_back(std::move(sender));
            bleSenders.push_back(nullptr);
        } else {
            auto sender = std::make_unique<FTSender>();
            if (!sender->init(panel.ip, panel.port)) {
                std::cerr << "Warning: Failed to init sender for panel "
                          << panel.name << " (" << panel.ip << ")" << std::endl;
            }
            senders.push_back(std::move(sender));
            bleSenders.push_back(nullptr);
        }
    }
    std::cerr << "Initialized " << config.panels.size() << " panel senders" << std::endl;

    // Initialize audio player (optional, shared across clips)
    AudioPlayer audioPlayer;
    if (!config.audio_device.empty()) {
        if (!audioPlayer.init(config.audio_device)) {
            std::cerr << "Warning: Audio output failed, continuing without audio" << std::endl;
        }
    }

    // Build MIDI note -> mapping lookup
    std::map<int, int> noteMappings;
    for (size_t i = 0; i < config.mappings.size(); i++) {
        noteMappings[config.mappings[i].note] = (int)i;
    }

    // Active clip player (audio-master orchestrator)
    std::unique_ptr<ClipPlayer> activeClip;
    std::string activeClipName;

    // Pre-allocate region extraction buffers (one per panel)
    std::vector<std::vector<uint8_t>> regionBuffers;
    for (const auto& panel : config.panels) {
        regionBuffers.emplace_back(panel.src_w * panel.src_h * 3, 0);
    }

    // Per-panel send throttle (for max_fps limiting)
    std::vector<std::chrono::steady_clock::time_point> lastPanelSend(config.panels.size());

    // Start MIDI input
    MidiInput midiInput;
    if (!midiInput.start()) {
        std::cerr << "Warning: MIDI input failed to start (continuing without MIDI)" << std::endl;
    }

    // Start status web server
    auto startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    StatusServer statusServer(config.web_port);
    statusServer.setConfig(&config);
    statusServer.setStartTime(startTime);
    if (midiInput.isRunning()) {
        statusServer.setMidiDevice(midiInput.getDeviceName());
    }
    statusServer.start();

    // Set up terminal raw mode for keyboard test mode
    struct termios oldTerm, newTerm;
    if (testMode) {
        tcgetattr(STDIN_FILENO, &oldTerm);
        newTerm = oldTerm;
        newTerm.c_lflag &= ~(ICANON | ECHO);
        newTerm.c_cc[VMIN] = 0;
        newTerm.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &newTerm);

        std::cerr << "\n=== KEYBOARD TEST MODE ===" << std::endl;
        std::cerr << "Keys 1-9, 0, -, = trigger clips (or F1-F12)" << std::endl;
        std::cerr << "Q = quit\n" << std::endl;
        for (size_t i = 0; i < config.mappings.size() && i < 12; i++) {
            const char* keys[] = {"1/F1","2/F2","3/F3","4/F4","5/F5","6/F6",
                                  "7/F7","8/F8","9/F9","0/F10","-/F11","=/F12"};
            std::cerr << "  " << keys[i] << " -> " << config.mappings[i].clip << std::endl;
        }
        std::cerr << std::endl;
    }

    std::cerr << "midi-ft-bridge running. Press Ctrl+C to stop." << std::endl;

    // Helper: send black to all panels
    auto sendBlackToAll = [&]() {
        for (size_t i = 0; i < config.panels.size(); i++) {
            if (config.panels[i].type == "ble" && bleSenders[i]) {
                bleSenders[i]->sendBlack(config.panels[i].src_w, config.panels[i].src_h);
            } else if (config.panels[i].type == "ble_udp" && senders[i]) {
                std::vector<uint8_t> black(config.panels[i].src_w * config.panels[i].src_h * 3, 0);
                senders[i]->sendRaw(black.data(), config.panels[i].src_w, config.panels[i].src_h);
            } else if (senders[i]) {
                senders[i]->sendBlack(config.panels[i].src_w, config.panels[i].src_h);
            }
        }
    };

    // Helper: trigger a clip by mapping index
    auto triggerMapping = [&](int mappingIdx) {
        if (mappingIdx < 0 || mappingIdx >= (int)config.mappings.size()) return;
        const auto& mapping = config.mappings[mappingIdx];
        std::string clipPath = config.clips_dir + "/" + mapping.clip;

        // Stop previous clip
        if (activeClip) {
            activeClip->stop();
            activeClip.reset();
        }

        auto player = std::make_unique<ClipPlayer>();
        if (player->open(clipPath, config.video_width, config.video_height, &audioPlayer)) {
            activeClipName = mapping.clip;
            activeClip = std::move(player);
            std::cerr << "Playing " << mapping.clip << " on all panels" << std::endl;
        }
    };

    // Test callback: simulate a MIDI note press from the web UI
    statusServer.setTestCallback([&](int note) {
        auto it = noteMappings.find(note);
        if (it == noteMappings.end()) return;
        triggerMapping(it->second);
    });

    // Main loop — no longer does decoding, just picks frames and sends
    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // Poll keyboard in test mode
        if (testMode) {
            int keyIdx = pollKeyboard();
            if (keyIdx == -2 && activeClip) {
                std::cerr << "ESC -> stop" << std::endl;
                activeClip->stop();
                activeClip.reset();
                activeClipName.clear();
                sendBlackToAll();
            } else if (keyIdx >= 0) {
                bool alreadyPlaying = activeClip &&
                    keyIdx < (int)config.mappings.size() &&
                    activeClipName == config.mappings[keyIdx].clip;
                if (!alreadyPlaying) {
                    std::cerr << "Key " << keyIdx << " -> trigger" << std::endl;
                    triggerMapping(keyIdx);
                }
            }
        }

        // Poll MIDI events
        MidiEvent event;
        while (midiInput.getNextEvent(event)) {
            MidiEventLog logEntry;
            logEntry.type = (event.type == MidiEvent::NOTE_ON) ? "NOTE_ON" : "NOTE_OFF";
            logEntry.note = event.note;
            logEntry.velocity = event.velocity;
            logEntry.channel = event.channel;
            logEntry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            statusServer.logMidiEvent(logEntry);

            if (event.type != MidiEvent::NOTE_ON) continue;
            if (config.midi_channel >= 0 && event.channel != config.midi_channel) continue;

            auto it = noteMappings.find(event.note);
            if (it == noteMappings.end()) continue;

            triggerMapping(it->second);
        }

        // Get current frame from clip player (synced to audio clock)
        if (activeClip) {
            const uint8_t* frame = activeClip->getCurrentFrame();

            if (frame) {
                // Extract each panel's region and send
                for (size_t i = 0; i < config.panels.size(); i++) {
                    const auto& panel = config.panels[i];

                    // Throttle: skip this panel if max_fps is set and interval hasn't elapsed
                    if (panel.max_fps > 0) {
                        double minInterval = 1.0 / panel.max_fps;
                        auto sinceLastSend = std::chrono::duration<double>(now - lastPanelSend[i]).count();
                        if (sinceLastSend < minInterval) continue;
                    }
                    lastPanelSend[i] = now;

                    extractRegion(frame, config.video_width,
                                  panel.src_x, panel.src_y,
                                  panel.src_w, panel.src_h,
                                  regionBuffers[i].data());
                    if (panel.type == "ble" && bleSenders[i]) {
                        bleSenders[i]->sendFrame(regionBuffers[i].data(), panel.src_w, panel.src_h);
                    } else if (panel.type == "ble_udp" && senders[i]) {
                        senders[i]->sendRaw(regionBuffers[i].data(), panel.src_w, panel.src_h);
                    } else if (senders[i]) {
                        senders[i]->send(regionBuffers[i].data(), panel.src_w, panel.src_h);
                    }
                }
            } else if (activeClip->isFinished()) {
                std::cerr << "Clip finished: " << activeClipName << std::endl;
                activeClip.reset();
                activeClipName.clear();
                sendBlackToAll();
            }
        }

        // Update status server with panel info
        std::vector<PanelStatus> panelStatus;
        for (size_t i = 0; i < config.panels.size(); i++) {
            PanelStatus ps;
            ps.name = config.panels[i].name;
            ps.ip = config.panels[i].ip;
            ps.port = config.panels[i].port;
            if (config.panels[i].type == "ble" && bleSenders[i]) {
                ps.framesSent = bleSenders[i]->getFramesSent();
                ps.bytesSent = 0;
                ps.enabled = bleSenders[i]->isConnected();
            } else if (senders[i]) {
                ps.framesSent = senders[i]->getFramesSent();
                ps.bytesSent = senders[i]->getBytesSent();
                ps.enabled = senders[i]->isEnabled();
            } else {
                ps.framesSent = 0;
                ps.bytesSent = 0;
                ps.enabled = false;
            }
            ps.activeClip = activeClip ? activeClipName : "";
            panelStatus.push_back(ps);
        }
        statusServer.updatePanelStatus(panelStatus);

        if (midiInput.isRunning()) {
            statusServer.setMidiDevice(midiInput.getDeviceName());
        }

        // Sleep to avoid busy-waiting (1ms is fine — we're not decoding here anymore)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Cleanup
    std::cerr << "\nShutting down..." << std::endl;

    if (testMode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
    }

    if (activeClip) {
        activeClip->stop();
        activeClip.reset();
    }

    sendBlackToAll();

    for (size_t i = 0; i < bleSenders.size(); i++) {
        if (bleSenders[i]) bleSenders[i]->stop();
    }

    audioPlayer.close();
    statusServer.stop();
    midiInput.stop();

    std::cerr << "midi-ft-bridge stopped." << std::endl;
    return 0;
}
