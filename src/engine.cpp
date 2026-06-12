// ====================================================================
//  Engine - Implementation
// ====================================================================
#include "engine.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace {
// True for panels we can SSH a shutdown to (FT Pi panels with a real IP).
bool isShutdownable(const PanelConfig& p) {
    return p.type == "ft" && !p.ip.empty() && p.ip != "127.0.0.1";
}

// Fire-and-forget SSH shutdown; returns a one-line "name (ip): sent|failed".
std::string sshShutdown(const PanelConfig& p) {
    std::string cmd = "ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@" +
                      p.ip + " 'sudo shutdown now' 2>&1 &";
    int ret = system(cmd.c_str());
    std::string line = p.name + " (" + p.ip + "): " + (ret == 0 ? "sent" : "failed");
    std::cerr << "Shutdown " << line << std::endl;
    return line + "\n";
}
}  // namespace

Engine::Engine() = default;

Engine::~Engine() {
    stop();
}

void Engine::extractRegion(const uint8_t* src, int srcWidth,
                           int sx, int sy, int sw, int sh,
                           uint8_t* dst) {
    for (int row = 0; row < sh; ++row) {
        std::memcpy(dst + row * sw * 3,
                    src + (sy + row) * srcWidth * 3 + sx * 3,
                    sw * 3);
    }
}

bool Engine::start(const std::string& configPath, bool statusServerEnabled) {
    if (m_started) return true;

    if (!m_config.load(configPath)) {
        std::cerr << "Engine: failed to load config from " << configPath << std::endl;
        return false;
    }

    // Initialize senders
    for (const auto& panel : m_config.panels) {
        if (panel.type == "ble") {
            auto ble = std::make_unique<BleSender>();
            ble->init(panel.ble_addr, panel.brightness, m_config.debug, panel.ble_name);
            m_senders.push_back(nullptr);
            m_bleSenders.push_back(std::move(ble));
        } else if (panel.type == "ble_udp") {
            auto sender = std::make_unique<FTSender>();
            if (!sender->init(panel.ip, panel.port)) {
                std::cerr << "Engine: ble_udp sender init failed for " << panel.name
                          << " (" << panel.ip << ":" << panel.port << ")" << std::endl;
            }
            m_senders.push_back(std::move(sender));
            m_bleSenders.push_back(nullptr);
        } else {
            auto sender = std::make_unique<FTSender>();
            if (!sender->init(panel.ip, panel.port)) {
                std::cerr << "Engine: sender init failed for " << panel.name
                          << " (" << panel.ip << ")" << std::endl;
            }
            m_senders.push_back(std::move(sender));
            m_bleSenders.push_back(nullptr);
        }
    }
    std::cerr << "Engine: " << m_config.panels.size() << " panel senders initialized" << std::endl;

    // Audio
    if (!m_config.audio_device.empty()) {
        if (!m_audioPlayer.init(m_config.audio_device)) {
            std::cerr << "Engine: audio init failed; continuing without audio" << std::endl;
        }
    }

    // Mapping lookup
    for (size_t i = 0; i < m_config.mappings.size(); ++i) {
        m_noteMappings[m_config.mappings[i].note] = static_cast<int>(i);
    }

    // Per-panel scratch buffers + throttle clocks
    m_regionBuffers.clear();
    m_lastPanelSend.clear();
    for (const auto& panel : m_config.panels) {
        m_regionBuffers.emplace_back(panel.src_w * panel.src_h * 3, 0);
    }
    m_lastPanelSend.resize(m_config.panels.size());

    // MIDI
    if (!m_midiInput.start()) {
        std::cerr << "Engine: MIDI input failed to start (continuing without MIDI)" << std::endl;
    }

    // Optional status server
    m_startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (statusServerEnabled) {
        m_statusServer = std::make_unique<StatusServer>(m_config.web_port);
        m_statusServer->setConfig(&m_config);
        m_statusServer->setStartTime(m_startTime);
        if (m_midiInput.isRunning()) {
            m_statusServer->setMidiDevice(m_midiInput.getDeviceName());
        }
        m_statusServer->setTestCallback([this](int note) { triggerNote(note); });
        m_statusServer->setStopCallback([this]() {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_shutdownCallback;
            }
            if (cb) cb();
        });
        m_statusServer->setShutdownPanelsCallback([this]() { return shutdownPanels(); });
        m_statusServer->start();
    }

    m_started = true;
    m_running = true;
    m_worker = std::thread(&Engine::workerLoop, this);
    return true;
}

void Engine::stop() {
    if (!m_started) return;
    m_autoPlay.store(false);
    m_autoPlayIndex.store(-1);
    m_running = false;
    if (m_worker.joinable()) m_worker.join();
    shutdownInternals();
    m_started = false;
}

void Engine::shutdownInternals() {
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_activeClip) {
            m_activeClip->stop();
            m_activeClip.reset();
        }
        m_activeClipName.clear();
    }

    sendBlackToAll();

    for (auto& ble : m_bleSenders) {
        if (ble) ble->stop();
    }

    m_audioPlayer.close();

    if (m_statusServer) {
        m_statusServer->stop();
        m_statusServer.reset();
    }

    m_midiInput.stop();

    m_senders.clear();
    m_bleSenders.clear();
    m_regionBuffers.clear();
    m_lastPanelSend.clear();
    m_noteMappings.clear();
}

void Engine::sendBlackToAll() {
    for (size_t i = 0; i < m_config.panels.size(); ++i) {
        const auto& panel = m_config.panels[i];
        if (panel.type == "ble" && m_bleSenders[i]) {
            m_bleSenders[i]->sendBlack(panel.src_w, panel.src_h);
        } else if (panel.type == "ble_udp" && m_senders[i]) {
            std::vector<uint8_t> black(panel.src_w * panel.src_h * 3, 0);
            m_senders[i]->sendRaw(black.data(), panel.src_w, panel.src_h);
        } else if (m_senders[i]) {
            m_senders[i]->sendBlack(panel.src_w, panel.src_h);
        }
    }
}

void Engine::triggerMapping(int mappingIdx) {
    if (mappingIdx < 0 || mappingIdx >= static_cast<int>(m_config.mappings.size())) return;
    // Remember where we are so auto-play continues from the last clip played,
    // whether it was triggered manually, by MIDI, or by the auto-advance.
    m_autoPlayIndex.store(mappingIdx);
    const auto& mapping = m_config.mappings[mappingIdx];
    std::string clipPath = m_config.clips_dir + "/" + mapping.clip;

    std::lock_guard<std::mutex> lock(m_clipMutex);
    if (m_activeClip) {
        m_activeClip->stop();
        m_activeClip.reset();
    }

    std::cerr << "Engine: trigger note=" << mapping.note << " clip=" << clipPath << std::endl;

    auto player = std::make_unique<ClipPlayer>();
    if (player->open(clipPath, m_config.video_width, m_config.video_height, &m_audioPlayer)) {
        m_activeClipName = mapping.clip;
        m_activeClip = std::move(player);
        std::cerr << "Engine: playing " << mapping.clip << std::endl;
    } else {
        std::cerr << "Engine: FAILED to open " << clipPath << " (cwd=";
        char cwd[1024]; if (getcwd(cwd, sizeof(cwd))) std::cerr << cwd; else std::cerr << "?";
        std::cerr << ")" << std::endl;
    }
}

void Engine::triggerNote(int note) {
    auto it = m_noteMappings.find(note);
    if (it == m_noteMappings.end()) return;
    triggerMapping(it->second);
}

void Engine::stopActiveClip() {
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_activeClip) {
            m_activeClip->stop();
            m_activeClip.reset();
        }
        m_activeClipName.clear();
    }
    sendBlackToAll();
}

void Engine::togglePause() {
    std::lock_guard<std::mutex> lock(m_clipMutex);
    if (m_activeClip) m_activeClip->togglePause();
}

bool Engine::isClipPaused() const {
    std::lock_guard<std::mutex> lock(m_clipMutex);
    return m_activeClip && m_activeClip->isPaused();
}

void Engine::setAutoPlay(bool on) {
    m_autoPlay.store(on);
    if (!on || m_config.mappings.empty()) return;

    // Kick the loop off now if nothing is playing; otherwise let the current
    // clip finish and the worker's auto-advance takes over from there.
    bool idle;
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        idle = !m_activeClip;
    }
    if (idle) {
        int start = m_autoPlayIndex.load();
        start = (start < 0) ? 0 : (start % static_cast<int>(m_config.mappings.size()));
        triggerMapping(start);
    }
}

std::string Engine::getActiveClipName() const {
    std::lock_guard<std::mutex> lock(m_clipMutex);
    return m_activeClipName;
}

void Engine::setFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_frameCallback = std::move(cb);
}

void Engine::setShutdownCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_shutdownCallback = std::move(cb);
}

std::string Engine::getMidiDeviceName() const {
    return m_midiInput.getDeviceName();
}

std::vector<PanelStatus> Engine::getPanelStatus() const {
    std::lock_guard<std::mutex> lock(m_panelStatusMutex);
    return m_lastPanelStatus;
}

std::string Engine::shutdownPanels() {
    std::string result;
    for (const auto& panel : m_config.panels) {
        if (isShutdownable(panel)) result += sshShutdown(panel);
    }
    return result;
}

std::string Engine::shutdownPanel(const std::string& name) {
    for (const auto& panel : m_config.panels) {
        if (panel.name == name) {
            if (isShutdownable(panel)) return sshShutdown(panel);
            return name + ": not shutdownable (not an FT panel)\n";
        }
    }
    return name + ": panel not found\n";
}

void Engine::workerLoop() {
    while (m_running.load()) {
        auto now = std::chrono::steady_clock::now();

        // Drain MIDI events
        MidiEvent event;
        while (m_midiInput.getNextEvent(event)) {
            if (m_statusServer) {
                MidiEventLog logEntry;
                logEntry.type     = (event.type == MidiEvent::NOTE_ON) ? "NOTE_ON" : "NOTE_OFF";
                logEntry.note     = event.note;
                logEntry.velocity = event.velocity;
                logEntry.channel  = event.channel;
                logEntry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                m_statusServer->logMidiEvent(logEntry);
            }

            if (event.type != MidiEvent::NOTE_ON) continue;
            if (m_config.midi_channel >= 0 && event.channel != m_config.midi_channel) continue;

            auto it = m_noteMappings.find(event.note);
            if (it == m_noteMappings.end()) continue;
            triggerMapping(it->second);
        }

        // Pull current frame from active clip and dispatch
        bool clipFinished = false;
        const uint8_t* frame = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_clipMutex);
            if (m_activeClip) {
                frame = m_activeClip->getCurrentFrame();
                if (!frame && m_activeClip->isFinished()) {
                    clipFinished = true;
                }
            }
        }

        if (frame) {
            // Notify subscribers (e.g. UI preview) with the canvas before extraction.
            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_frameCallback;
            }
            if (cb) cb(frame, m_config.video_width, m_config.video_height);

            for (size_t i = 0; i < m_config.panels.size(); ++i) {
                const auto& panel = m_config.panels[i];

                if (panel.max_fps > 0) {
                    double minInterval = 1.0 / panel.max_fps;
                    auto sinceLast = std::chrono::duration<double>(now - m_lastPanelSend[i]).count();
                    if (sinceLast < minInterval) continue;
                }
                m_lastPanelSend[i] = now;

                extractRegion(frame, m_config.video_width,
                              panel.src_x, panel.src_y,
                              panel.src_w, panel.src_h,
                              m_regionBuffers[i].data());

                if (panel.type == "ble" && m_bleSenders[i]) {
                    m_bleSenders[i]->sendFrame(m_regionBuffers[i].data(), panel.src_w, panel.src_h);
                } else if (panel.type == "ble_udp" && m_senders[i]) {
                    m_senders[i]->sendRaw(m_regionBuffers[i].data(), panel.src_w, panel.src_h);
                } else if (m_senders[i]) {
                    m_senders[i]->send(m_regionBuffers[i].data(), panel.src_w, panel.src_h);
                }
            }
        } else if (clipFinished) {
            std::string finishedName;
            {
                std::lock_guard<std::mutex> lock(m_clipMutex);
                finishedName = m_activeClipName;
                m_activeClip.reset();
                m_activeClipName.clear();
            }
            std::cerr << "Engine: clip finished: " << finishedName << std::endl;

            // Test/auto-play mode: advance to the next mapping and loop forever.
            if (m_autoPlay.load() && !m_config.mappings.empty()) {
                int next = (m_autoPlayIndex.load() + 1) %
                           static_cast<int>(m_config.mappings.size());
                triggerMapping(next);
            } else {
                sendBlackToAll();
            }
        }

        // Build the per-panel status snapshot (consumed by both the native UI
        // via getPanelStatus() and the optional HTTP status server).
        {
            std::vector<PanelStatus> panelStatus;
            std::string clipName;
            {
                std::lock_guard<std::mutex> lock(m_clipMutex);
                clipName = m_activeClipName;
            }
            for (size_t i = 0; i < m_config.panels.size(); ++i) {
                PanelStatus ps;
                ps.name = m_config.panels[i].name;
                ps.ip   = m_config.panels[i].ip;
                ps.port = m_config.panels[i].port;
                ps.type = m_config.panels[i].type;
                if (m_config.panels[i].type == "ble" && m_bleSenders[i]) {
                    ps.framesSent = m_bleSenders[i]->getFramesSent();
                    ps.bytesSent  = 0;
                    ps.enabled    = m_bleSenders[i]->isConnected();
                } else if (m_senders[i]) {
                    ps.framesSent = m_senders[i]->getFramesSent();
                    ps.bytesSent  = m_senders[i]->getBytesSent();
                    ps.enabled    = m_senders[i]->isEnabled();
                } else {
                    ps.framesSent = 0;
                    ps.bytesSent  = 0;
                    ps.enabled    = false;
                }
                ps.activeClip = clipName;
                panelStatus.push_back(ps);
            }
            {
                std::lock_guard<std::mutex> lock(m_panelStatusMutex);
                m_lastPanelStatus = panelStatus;
            }
            if (m_statusServer) {
                m_statusServer->updatePanelStatus(panelStatus);
                if (m_midiInput.isRunning()) {
                    m_statusServer->setMidiDevice(m_midiInput.getDeviceName());
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
