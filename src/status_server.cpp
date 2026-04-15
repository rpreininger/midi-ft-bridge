// ====================================================================
//  Status Server - HTTP status/control interface implementation
// ====================================================================

#include "status_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>

StatusServer::StatusServer(int port) : m_port(port) {}

StatusServer::~StatusServer() {
    stop();
}

bool StatusServer::start() {
    if (m_running) return true;
    m_running = true;
    m_thread = std::thread(&StatusServer::serverThread, this);
    return true;
}

void StatusServer::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void StatusServer::updatePanelStatus(const std::vector<PanelStatus>& panels) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_panelStatus = panels;
}

void StatusServer::logMidiEvent(const MidiEventLog& event) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_midiLog.push_back(event);
    if ((int)m_midiLog.size() > MAX_MIDI_LOG) {
        m_midiLog.erase(m_midiLog.begin());
    }
}

void StatusServer::setMidiDevice(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_midiDevice = name;
}

void StatusServer::setStartTime(uint64_t epochMs) {
    m_startTime = epochMs;
}

void StatusServer::serverThread() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "StatusServer: Failed to create socket" << std::endl;
        m_running = false;
        return;
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "StatusServer: Failed to bind to port " << m_port << std::endl;
        close(serverSocket);
        m_running = false;
        return;
    }

    listen(serverSocket, 5);
    std::cerr << "StatusServer: Running on http://0.0.0.0:" << m_port << std::endl;

    while (m_running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSocket >= 0) {
            handleClient(clientSocket);
        }
    }

    close(serverSocket);
}

void StatusServer::handleClient(int clientSocket) {
    char buffer[2048] = {0};
    read(clientSocket, buffer, sizeof(buffer) - 1);

    std::string request(buffer);
    std::string response;

    if (request.find("GET /api/status") != std::string::npos) {
        std::string json = generateStatusJSON();
        response = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Access-Control-Allow-Origin: *\r\n\r\n" + json;
    }
    else if (request.find("GET /api/shutdown-panels") != std::string::npos) {
        // Shutdown all FT panels via SSH
        std::string result;
        if (m_config) {
            for (const auto& panel : m_config->panels) {
                if (panel.type == "ft" && panel.ip != "127.0.0.1") {
                    std::string cmd = "ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@" + panel.ip + " 'sudo shutdown now' 2>&1 &";
                    int ret = system(cmd.c_str());
                    result += panel.name + " (" + panel.ip + "): " + (ret == 0 ? "sent" : "failed") + "\n";
                    std::cerr << "Shutdown " << panel.name << " (" << panel.ip << "): " << (ret == 0 ? "sent" : "failed") << std::endl;
                }
            }
        }
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nShutdown sent:\n" + result;
    }
    else if (request.find("GET /api/shutdown-hub") != std::string::npos) {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHub shutting down...";
        write(clientSocket, response.c_str(), response.length());
        close(clientSocket);
        system("sudo shutdown now &");
        return;
    }
    else if (request.find("GET /api/test?note=") != std::string::npos) {
        size_t pos = request.find("note=");
        int note = atoi(request.c_str() + pos + 5);
        if (m_testCallback) {
            m_testCallback(note);
        }
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nTriggered note " + std::to_string(note);
    }
    else {
        std::string html = generateHTML();
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n" + html;
    }

    write(clientSocket, response.c_str(), response.length());
    close(clientSocket);
}

std::string StatusServer::generateStatusJSON() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    std::ostringstream json;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    json << "{";

    // Uptime
    uint64_t uptimeMs = (m_startTime > 0) ? (now - m_startTime) : 0;
    json << "\"uptime_seconds\":" << (uptimeMs / 1000);

    // MIDI device
    json << ",\"midi_device\":\"" << m_midiDevice << "\"";

    // Panels
    json << ",\"panels\":[";
    for (size_t i = 0; i < m_panelStatus.size(); i++) {
        if (i > 0) json << ",";
        const auto& p = m_panelStatus[i];
        json << "{\"name\":\"" << p.name << "\""
             << ",\"ip\":\"" << p.ip << "\""
             << ",\"port\":" << p.port
             << ",\"frames_sent\":" << p.framesSent
             << ",\"bytes_sent\":" << p.bytesSent
             << ",\"enabled\":" << (p.enabled ? "true" : "false")
             << ",\"active_clip\":\"" << p.activeClip << "\""
             << "}";
    }
    json << "]";

    // Recent MIDI events (newest first)
    json << ",\"midi_events\":[";
    for (int i = (int)m_midiLog.size() - 1; i >= 0; i--) {
        if (i < (int)m_midiLog.size() - 1) json << ",";
        const auto& e = m_midiLog[i];
        json << "{\"type\":\"" << e.type << "\""
             << ",\"note\":" << e.note
             << ",\"velocity\":" << e.velocity
             << ",\"channel\":" << e.channel
             << ",\"time\":" << e.timestamp
             << "}";
    }
    json << "]";

    // Mappings from config
    if (m_config) {
        json << ",\"mappings\":[";
        for (size_t i = 0; i < m_config->mappings.size(); i++) {
            if (i > 0) json << ",";
            const auto& m = m_config->mappings[i];
            json << "{\"note\":" << m.note
                 << ",\"panel\":\"" << m.panel << "\""
                 << ",\"clip\":\"" << m.clip << "\""
                 << "}";
        }
        json << "]";
    }

    json << "}";
    return json.str();
}

std::string StatusServer::generateHTML() {
    std::ostringstream html;
    html << R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
    <title>MIDI-FT Bridge Status</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: -apple-system, 'Segoe UI', Arial, sans-serif; background: #0f0f1a; color: #e0e0e0; padding: 20px; }
        h1 { color: #00d4ff; text-align: center; margin-bottom: 8px; font-size: 24px; }
        .subtitle { text-align: center; color: #666; margin-bottom: 24px; font-size: 14px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; margin-bottom: 16px; }
        .card { background: #1a1a2e; border-radius: 12px; padding: 16px; border: 1px solid #2a2a4a; }
        .card h2 { font-size: 14px; text-transform: uppercase; letter-spacing: 1px; color: #888; margin-bottom: 12px; }
        .stat { display: flex; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid #2a2a4a; }
        .stat:last-child { border-bottom: none; }
        .stat-label { color: #aaa; }
        .stat-value { color: #fff; font-weight: 600; }
        .panel-row { display: flex; align-items: center; padding: 10px; margin: 6px 0; background: #16213e; border-radius: 8px; }
        .panel-dot { width: 12px; height: 12px; border-radius: 50%; margin-right: 12px; flex-shrink: 0; }
        .panel-dot.active { background: #00ff88; box-shadow: 0 0 8px #00ff8866; }
        .panel-dot.idle { background: #444; }
        .panel-info { flex: 1; }
        .panel-name { font-weight: 700; font-size: 16px; }
        .panel-detail { font-size: 12px; color: #888; margin-top: 2px; }
        .panel-clip { color: #ff6b9d; font-size: 13px; margin-top: 4px; }
        .midi-log { max-height: 280px; overflow-y: auto; font-family: 'Consolas', 'Monaco', monospace; font-size: 13px; }
        .midi-entry { padding: 4px 8px; border-radius: 4px; margin: 2px 0; }
        .midi-entry.note-on { background: #1a3a2a; color: #00ff88; }
        .midi-entry.note-off { background: #2a1a1a; color: #ff6666; }
        .mapping-row { display: flex; align-items: center; padding: 8px; margin: 4px 0; background: #16213e; border-radius: 6px; font-size: 14px; }
        .mapping-note { background: #0f3460; padding: 2px 8px; border-radius: 4px; font-weight: 700; margin-right: 10px; min-width: 50px; text-align: center; }
        .mapping-arrow { color: #555; margin: 0 8px; }
        .mapping-panel { color: #00d4ff; margin-right: 8px; }
        .mapping-clip { color: #ff6b9d; }
        .test-btn { background: #0f3460; border: 1px solid #1a5a9e; color: #fff; padding: 3px 10px; border-radius: 4px; cursor: pointer; font-size: 12px; margin-left: auto; }
        .test-btn:hover { background: #1a5a9e; }
        .shutdown-section { margin-top: 16px; display: flex; gap: 12px; justify-content: center; flex-wrap: wrap; }
        .shutdown-btn { background: #4a1a1a; border: 1px solid #8a3333; color: #ff6666; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-size: 14px; font-weight: 600; }
        .shutdown-btn:hover { background: #6a2a2a; border-color: #ff4444; }
        .shutdown-btn.hub { background: #5a1a1a; border-color: #aa3333; color: #ff4444; }
        .badge { display: inline-block; padding: 2px 8px; border-radius: 10px; font-size: 11px; font-weight: 600; }
        .badge-ok { background: #1a3a2a; color: #00ff88; }
        .badge-warn { background: #3a3a1a; color: #ffcc00; }
        .badge-off { background: #2a1a1a; color: #ff6666; }
    </style>
</head>
<body>
    <h1>MIDI-FT Bridge</h1>
    <div class="subtitle">Roland Fantom 06 &rarr; Flaschen-Taschen LED Panels</div>

    <div class="grid">
        <div class="card">
            <h2>System</h2>
            <div class="stat">
                <span class="stat-label">Uptime</span>
                <span class="stat-value" id="uptime">--</span>
            </div>
            <div class="stat">
                <span class="stat-label">MIDI Device</span>
                <span class="stat-value" id="midiDevice">--</span>
            </div>
            <div class="stat">
                <span class="stat-label">MIDI Events</span>
                <span class="stat-value" id="midiCount">0</span>
            </div>
        </div>

        <div class="card">
            <h2>Panels</h2>
            <div id="panels">
                <div style="color:#666;padding:10px;">Loading...</div>
            </div>
        </div>
    </div>

    <div class="grid">
        <div class="card">
            <h2>Mappings</h2>
            <div id="mappings"></div>
        </div>

        <div class="card">
            <h2>MIDI Event Log</h2>
            <div class="midi-log" id="midiLog">
                <div style="color:#666;padding:10px;">Waiting for events...</div>
            </div>
        </div>
    </div>

    <div class="shutdown-section">
        <button class="shutdown-btn" onclick="shutdownPanels()">Shutdown All Panels</button>
        <button class="shutdown-btn hub" onclick="shutdownHub()">Shutdown Hub</button>
    </div>

    <script>
        function shutdownPanels() {
            if (!confirm('Shutdown all FT panels?')) return;
            fetch('/api/shutdown-panels').then(function(r) { return r.text(); }).then(function(t) { alert(t); });
        }
        function shutdownHub() {
            if (!confirm('Shutdown the hub (Pi)? This will stop all services!')) return;
            fetch('/api/shutdown-hub').then(function(r) { return r.text(); }).then(function(t) { alert(t); });
        }

        function formatUptime(seconds) {
            var h = Math.floor(seconds / 3600);
            var m = Math.floor((seconds % 3600) / 60);
            var s = seconds % 60;
            if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
            if (m > 0) return m + 'm ' + s + 's';
            return s + 's';
        }

        function formatBytes(bytes) {
            if (bytes > 1073741824) return (bytes / 1073741824).toFixed(1) + ' GB';
            if (bytes > 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
            if (bytes > 1024) return (bytes / 1024).toFixed(1) + ' KB';
            return bytes + ' B';
        }

        function noteName(n) {
            var names = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
            return names[n % 12] + (Math.floor(n / 12) - 1);
        }

        function triggerTest(note) {
            fetch('/api/test?note=' + note).then(function(r) { return r.text(); });
        }

        function refresh() {
            fetch('/api/status')
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    document.getElementById('uptime').textContent = formatUptime(data.uptime_seconds);

                    var dev = data.midi_device || 'Not connected';
                    var badge = data.midi_device ? '<span class="badge badge-ok">Connected</span>' : '<span class="badge badge-off">Disconnected</span>';
                    document.getElementById('midiDevice').innerHTML = dev + ' ' + badge;

                    // Panels
                    var ph = '';
                    var totalEvents = 0;
                    for (var i = 0; i < data.panels.length; i++) {
                        var p = data.panels[i];
                        var playing = p.active_clip && p.active_clip.length > 0;
                        ph += '<div class="panel-row">';
                        ph += '<div class="panel-dot ' + (playing ? 'active' : 'idle') + '"></div>';
                        ph += '<div class="panel-info">';
                        ph += '<div class="panel-name">Panel ' + p.name + '</div>';
                        ph += '<div class="panel-detail">' + p.ip + ':' + p.port + ' &middot; ' + p.frames_sent + ' frames &middot; ' + formatBytes(p.bytes_sent) + '</div>';
                        if (playing) ph += '<div class="panel-clip">Playing: ' + p.active_clip + '</div>';
                        ph += '</div></div>';
                    }
                    document.getElementById('panels').innerHTML = ph || '<div style="color:#666;padding:10px;">No panels configured</div>';

                    // Mappings
                    var mh = '';
                    if (data.mappings) {
                        for (var j = 0; j < data.mappings.length; j++) {
                            var m = data.mappings[j];
                            mh += '<div class="mapping-row">';
                            mh += '<span class="mapping-note">' + noteName(m.note) + ' (' + m.note + ')</span>';
                            mh += '<span class="mapping-arrow">&rarr;</span>';
                            mh += '<span class="mapping-panel">' + (m.panel === 'all' ? 'ALL' : 'Panel ' + m.panel) + '</span>';
                            mh += '<span class="mapping-arrow">&rarr;</span>';
                            mh += '<span class="mapping-clip">' + m.clip + '</span>';
                            mh += '<button class="test-btn" onclick="triggerTest(' + m.note + ')">Test</button>';
                            mh += '</div>';
                        }
                    }
                    document.getElementById('mappings').innerHTML = mh || '<div style="color:#666;padding:10px;">No mappings</div>';

                    // MIDI log
                    if (data.midi_events && data.midi_events.length > 0) {
                        var lh = '';
                        totalEvents = data.midi_events.length;
                        for (var k = 0; k < data.midi_events.length && k < 30; k++) {
                            var e = data.midi_events[k];
                            var cls = e.type === 'NOTE_ON' ? 'note-on' : 'note-off';
                            lh += '<div class="midi-entry ' + cls + '">';
                            lh += e.type + ' ' + noteName(e.note) + ' (' + e.note + ') vel=' + e.velocity + ' ch=' + e.channel;
                            lh += '</div>';
                        }
                        document.getElementById('midiLog').innerHTML = lh;
                    }
                    document.getElementById('midiCount').textContent = totalEvents;
                })
                .catch(function() {});
        }

        refresh();
        setInterval(refresh, 1000);
    </script>
</body>
</html>
)HTMLPAGE";

    return html.str();
}
