// ====================================================================
//  midi-ft-bridge — CLI entry point
//
//  All orchestration lives in Engine. main() handles CLI args, signal
//  handling, optional keyboard test mode, and lifecycle.
// ====================================================================

#include "engine.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/) {
    g_running = false;
}

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --config <path>   Config file (default: config.json)\n"
              << "  --test            Keyboard test mode (1-9/0/-/= or F1-F12 trigger clips)\n"
              << "  --help            Show this help\n";
}

// Returns mapping index 0-11 for digit/F-keys, -1 for none, -2 for ESC,
// -3 for SPACE.
static int pollKeyboard() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return -1;

    char buf[8] = {0};
    int n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n < 1) return -1;

    if (n == 1) {
        if (buf[0] >= '1' && buf[0] <= '9') return buf[0] - '1';
        if (buf[0] == '0') return 9;
        if (buf[0] == '-') return 10;
        if (buf[0] == '=') return 11;
        if (buf[0] == '\x1b') return -2;
        if (buf[0] == ' ') return -3;
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

    for (int i = 1; i < argc; ++i) {
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

    Engine engine;
    engine.setShutdownCallback([]() { g_running = false; });

    if (!engine.start(configPath, /*statusServerEnabled=*/true)) {
        return 1;
    }

    struct termios oldTerm{}, newTerm{};
    if (testMode) {
        tcgetattr(STDIN_FILENO, &oldTerm);
        newTerm = oldTerm;
        newTerm.c_lflag &= ~(ICANON | ECHO);
        newTerm.c_cc[VMIN]  = 0;
        newTerm.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &newTerm);

        std::cerr << "\n=== KEYBOARD TEST MODE ===" << std::endl;
        std::cerr << "Keys 1-9, 0, -, = trigger clips (or F1-F12)" << std::endl;
        std::cerr << "SPACE = pause/resume, ESC = stop clip, Q = quit\n" << std::endl;
        const auto& mappings = engine.getConfig().mappings;
        const char* keys[] = {"1/F1","2/F2","3/F3","4/F4","5/F5","6/F6",
                              "7/F7","8/F8","9/F9","0/F10","-/F11","=/F12"};
        for (size_t i = 0; i < mappings.size() && i < 12; ++i) {
            std::cerr << "  " << keys[i] << " -> " << mappings[i].clip << std::endl;
        }
        std::cerr << std::endl;
    }

    std::cerr << "midi-ft-bridge running. Press Ctrl+C to stop." << std::endl;

    while (g_running && engine.isRunning()) {
        if (testMode) {
            int keyIdx = pollKeyboard();
            if (keyIdx == -2) {
                if (!engine.getActiveClipName().empty()) {
                    std::cerr << "ESC -> stop" << std::endl;
                    engine.stopActiveClip();
                }
            } else if (keyIdx == -3) {
                if (!engine.getActiveClipName().empty()) {
                    engine.togglePause();
                    std::cerr << (engine.isClipPaused() ? "SPACE -> pause" : "SPACE -> resume") << std::endl;
                }
            } else if (keyIdx >= 0) {
                const auto& mappings = engine.getConfig().mappings;
                bool alreadyPlaying = keyIdx < (int)mappings.size() &&
                                      engine.getActiveClipName() == mappings[keyIdx].clip;
                if (!alreadyPlaying) {
                    std::cerr << "Key " << keyIdx << " -> trigger" << std::endl;
                    engine.triggerMapping(keyIdx);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cerr << "\nShutting down..." << std::endl;

    if (testMode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
    }

    engine.stop();
    std::cerr << "midi-ft-bridge stopped." << std::endl;
    return 0;
}
