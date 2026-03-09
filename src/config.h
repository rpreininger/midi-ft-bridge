// ====================================================================
//  Config - JSON configuration parser for midi-ft-bridge
// ====================================================================
#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

struct PanelConfig {
    std::string name;
    std::string ip;
    int port = 1337;
    int src_x = 0;   // Source region X offset in decoded frame
    int src_y = 0;   // Source region Y offset in decoded frame
    int src_w = 0;   // Source region width (0 = use video_width)
    int src_h = 0;   // Source region height (0 = use video_height)
    int max_fps = 0;  // Max send rate for this panel (0 = no limit, use clip FPS)
};

struct MappingConfig {
    int note;           // MIDI note number 0-127
    std::string panel;  // Panel name or "all"
    std::string clip;   // Filename in clips_dir
};

struct Config {
    std::vector<PanelConfig> panels;
    std::string clips_dir = "./clips";
    std::vector<MappingConfig> mappings;
    int default_fps = 25;
    int video_width = 256;
    int video_height = 128;
    int web_port = 8080;
    int midi_channel = -1;  // MIDI channel filter (0-15, -1 = any). Channel 10 = 9 (0-indexed)

    bool load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Config: Failed to open " << path << std::endl;
            return false;
        }

        std::string json((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
        file.close();

        // Simple JSON parser - good enough for our flat config
        panels.clear();
        mappings.clear();

        // Parse panels array
        size_t panelsStart = json.find("\"panels\"");
        if (panelsStart != std::string::npos) {
            size_t arrStart = json.find('[', panelsStart);
            size_t arrEnd = findMatchingBracket(json, arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = json.substr(arrStart, arrEnd - arrStart + 1);
                size_t pos = 0;
                while ((pos = arr.find('{', pos)) != std::string::npos) {
                    size_t objEnd = arr.find('}', pos);
                    if (objEnd == std::string::npos) break;
                    std::string obj = arr.substr(pos, objEnd - pos + 1);

                    PanelConfig panel;
                    panel.name = extractString(obj, "name");
                    panel.ip = extractString(obj, "ip");
                    panel.port = extractInt(obj, "port", 1337);
                    panel.src_x = extractInt(obj, "src_x", 0);
                    panel.src_y = extractInt(obj, "src_y", 0);
                    panel.src_w = extractInt(obj, "src_w", 0);
                    panel.src_h = extractInt(obj, "src_h", 0);
                    panel.max_fps = extractInt(obj, "max_fps", 0);
                    panels.push_back(panel);

                    pos = objEnd + 1;
                }
            }
        }

        // Parse mappings array
        size_t mappingsStart = json.find("\"mappings\"");
        if (mappingsStart != std::string::npos) {
            size_t arrStart = json.find('[', mappingsStart);
            size_t arrEnd = findMatchingBracket(json, arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = json.substr(arrStart, arrEnd - arrStart + 1);
                size_t pos = 0;
                while ((pos = arr.find('{', pos)) != std::string::npos) {
                    size_t objEnd = arr.find('}', pos);
                    if (objEnd == std::string::npos) break;
                    std::string obj = arr.substr(pos, objEnd - pos + 1);

                    MappingConfig mapping;
                    mapping.note = extractInt(obj, "note", -1);
                    mapping.panel = extractString(obj, "panel");
                    mapping.clip = extractString(obj, "clip");
                    if (mapping.note >= 0) {
                        mappings.push_back(mapping);
                    }

                    pos = objEnd + 1;
                }
            }
        }

        // Parse scalar values
        clips_dir = extractString(json, "clips_dir");
        if (clips_dir.empty()) clips_dir = "./clips";

        default_fps = extractInt(json, "default_fps", 25);
        video_width = extractInt(json, "video_width", 256);
        video_height = extractInt(json, "video_height", 128);
        web_port = extractInt(json, "web_port", 8080);
        midi_channel = extractInt(json, "midi_channel", -1);

        // Default panel src_w/src_h to full video dimensions if not set
        for (auto& panel : panels) {
            if (panel.src_w <= 0) panel.src_w = video_width;
            if (panel.src_h <= 0) panel.src_h = video_height;
        }

        std::cerr << "Config: Loaded " << panels.size() << " panels, "
                  << mappings.size() << " mappings";
        if (midi_channel >= 0) {
            std::cerr << ", MIDI channel filter: " << (midi_channel + 1);
        }
        std::cerr << std::endl;
        return true;
    }

    // Find a panel index by name, returns -1 if not found
    int findPanel(const std::string& name) const {
        for (size_t i = 0; i < panels.size(); i++) {
            if (panels[i].name == name) return (int)i;
        }
        return -1;
    }

private:
    static size_t findMatchingBracket(const std::string& s, size_t start) {
        if (start == std::string::npos || s[start] != '[') return std::string::npos;
        int depth = 1;
        for (size_t i = start + 1; i < s.size(); i++) {
            if (s[i] == '[') depth++;
            else if (s[i] == ']') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }

    static std::string extractString(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";

        pos = json.find(':', pos + search.length());
        if (pos == std::string::npos) return "";

        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";

        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";

        return json.substr(pos + 1, end - pos - 1);
    }

    static int extractInt(const std::string& json, const std::string& key, int defaultVal) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return defaultVal;

        pos = json.find(':', pos + search.length());
        if (pos == std::string::npos) return defaultVal;

        // Skip whitespace
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

        return atoi(json.c_str() + pos);
    }
};
