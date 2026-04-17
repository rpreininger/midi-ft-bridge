// ====================================================================
//  Panel Viewer - SDL2 FT panel simulator for local development
//
//  Reads config.json, listens on each panel's UDP port on localhost,
//  and renders incoming frames in a single window at their correct
//  layout positions with pixel scaling.
//
//  Usage: ./panel_viewer [--config config_local.json] [--scale 4]
// ====================================================================

#include "config.h"

#include <SDL.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

// Per-panel state
struct PanelView {
    std::string name;
    int port;
    int src_x, src_y;   // position in the canvas (from config)
    int src_w, src_h;   // panel dimensions
    int udpSocket;

    std::mutex frameMutex;
    std::vector<uint8_t> frameBuffer;  // latest RGB24 frame
    bool hasFrame;
};

// Parse PPM P6 header with optional #FT offset comment
// Returns true if valid, fills out width/height/offsetX/offsetY/dataOffset
static bool parsePPMHeader(const uint8_t* data, int len,
                           int& width, int& height,
                           int& offsetX, int& offsetY,
                           int& dataOffset) {
    if (len < 10) return false;
    if (data[0] != 'P' || data[1] != '6') return false;

    // Parse header text
    std::string header(reinterpret_cast<const char*>(data),
                       std::min(len, 256));

    offsetX = 0;
    offsetY = 0;
    width = 0;
    height = 0;

    size_t pos = 2; // skip "P6"

    // Skip whitespace/newlines and parse comments/dimensions
    int numbersFound = 0;
    int numbers[3] = {0, 0, 0}; // width, height, maxval

    while (pos < header.size() && numbersFound < 3) {
        // Skip whitespace
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\n' || header[pos] == '\r' || header[pos] == '\t'))
            pos++;

        if (pos >= header.size()) return false;

        // Comment line
        if (header[pos] == '#') {
            // Check for #FT: x y
            if (header.substr(pos, 4) == "#FT:") {
                if (sscanf(header.c_str() + pos + 4, " %d %d", &offsetX, &offsetY) != 2) {
                    offsetX = 0;
                    offsetY = 0;
                }
            }
            // Skip to end of line
            while (pos < header.size() && header[pos] != '\n') pos++;
            continue;
        }

        // Parse number
        if (header[pos] >= '0' && header[pos] <= '9') {
            numbers[numbersFound] = 0;
            while (pos < header.size() && header[pos] >= '0' && header[pos] <= '9') {
                numbers[numbersFound] = numbers[numbersFound] * 10 + (header[pos] - '0');
                pos++;
            }
            numbersFound++;
        } else {
            return false;
        }
    }

    if (numbersFound < 3) return false;

    width = numbers[0];
    height = numbers[1];
    // numbers[2] = maxval (should be 255)

    // Skip exactly one whitespace byte after maxval
    if (pos < header.size()) pos++;

    dataOffset = (int)pos;
    return (dataOffset + width * height * 3) <= len;
}

// UDP listener thread: receives FT packets and updates panel frame buffer
static void udpListener(PanelView* panel, std::atomic<bool>* running) {
    uint8_t buf[65536];

    while (*running) {
        int n = recvfrom(panel->udpSocket, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int width, height, offsetX, offsetY, dataOffset;
        if (parsePPMHeader(buf, n, width, height, offsetX, offsetY, dataOffset)) {
            // PPM P6 frame — blit tile into frame buffer at FT offset
            const uint8_t* pixels = buf + dataOffset;

            std::lock_guard<std::mutex> lock(panel->frameMutex);

            // Ensure frame buffer is allocated
            if (panel->frameBuffer.size() != (size_t)(panel->src_w * panel->src_h * 3)) {
                panel->frameBuffer.resize(panel->src_w * panel->src_h * 3, 0);
            }

            // Blit the tile into the panel frame buffer at the FT offset
            for (int row = 0; row < height; row++) {
                int destY = offsetY + row;
                if (destY < 0 || destY >= panel->src_h) continue;
                int copyW = std::min(width, panel->src_w - offsetX);
                if (copyW <= 0 || offsetX < 0) continue;

                memcpy(panel->frameBuffer.data() + (destY * panel->src_w + offsetX) * 3,
                       pixels + row * width * 3,
                       copyW * 3);
            }
            panel->hasFrame = true;
        }
    }
}

int main(int argc, char* argv[]) {
    std::string configPath = "config.json";
    int scale = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1) scale = 1;
            if (scale > 16) scale = 16;
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cerr << "Usage: " << argv[0] << " [--config path] [--scale N]\n"
                      << "  --config  Config file (default: config.json)\n"
                      << "  --scale   Pixel scale factor (default: 4)\n";
            return 0;
        }
    }

    // Load config
    Config config;
    if (!config.load(configPath)) {
        std::cerr << "Failed to load config from " << configPath << std::endl;
        return 1;
    }

    // Set up panels (unique_ptr because PanelView has a mutex)
    std::vector<std::unique_ptr<PanelView>> panels;
    for (const auto& pc : config.panels) {
        auto pv = std::make_unique<PanelView>();
        pv->name = pc.name;
        pv->port = pc.port;
        pv->src_x = pc.src_x;
        pv->src_y = pc.src_y;
        pv->src_w = pc.src_w;
        pv->src_h = pc.src_h;
        pv->hasFrame = false;

        // Create UDP socket
        pv->udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (pv->udpSocket < 0) {
            std::cerr << "Failed to create socket for panel " << pv->name << std::endl;
            continue;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(pv->udpSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(pv->udpSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

        // Non-blocking
        int flags = fcntl(pv->udpSocket, F_GETFL, 0);
        fcntl(pv->udpSocket, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(pv->port);

        if (bind(pv->udpSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind port " << pv->port << " for panel " << pv->name
                      << ": " << strerror(errno) << std::endl;
            close(pv->udpSocket);
            pv->udpSocket = -1;
            continue;
        }

        pv->frameBuffer.resize(pv->src_w * pv->src_h * 3, 0);
        panels.push_back(std::move(pv));
    }

    if (panels.empty()) {
        std::cerr << "No panels configured." << std::endl;
        return 1;
    }

    // Calculate canvas size from the video dimensions
    int canvasW = config.video_width;
    int canvasH = config.video_height;

    std::cerr << "Panel Viewer: " << panels.size() << " panels, "
              << canvasW << "x" << canvasH << " canvas, "
              << scale << "x scale" << std::endl;
    for (const auto& p : panels) {
        std::cerr << "  " << p->name << ": " << p->src_w << "x" << p->src_h
                  << " at (" << p->src_x << "," << p->src_y << ")"
                  << " port " << p->port << std::endl;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "MIDI-FT Panel Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        canvasW * scale, canvasH * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    // Scaled canvas: each source pixel becomes a scale x scale block
    // with a 1px dark gap, simulating LED pixel appearance
    int scaledW = canvasW * scale;
    int scaledH = canvasH * scale;
    int pixSize = scale - 1;  // lit pixel size (1px gap)

    SDL_Texture* texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        scaledW, scaledH);

    // Scaled canvas pixel buffer (rendered at full size, no SDL scaling)
    std::vector<uint8_t> canvas(scaledW * scaledH * 3, 0);

    // Start UDP listener threads
    std::atomic<bool> running{true};
    std::vector<std::thread> listenerThreads;
    for (auto& panel : panels) {
        if (panel->udpSocket >= 0) {
            listenerThreads.emplace_back(udpListener, panel.get(), &running);
        }
    }

    // Render a source pixel as a (pixSize x pixSize) block in the scaled canvas.
    // The remaining 1px gap stays black, simulating LED pixel appearance.
    auto renderPixel = [&](int srcX, int srcY, uint8_t r, uint8_t g, uint8_t b) {
        int baseX = srcX * scale;
        int baseY = srcY * scale;
        for (int dy = 0; dy < pixSize; dy++) {
            int py = baseY + dy;
            if (py < 0 || py >= scaledH) continue;
            for (int dx = 0; dx < pixSize; dx++) {
                int px = baseX + dx;
                if (px < 0 || px >= scaledW) continue;
                int idx = (py * scaledW + px) * 3;
                canvas[idx] = r;
                canvas[idx + 1] = g;
                canvas[idx + 2] = b;
            }
        }
    };

    std::cerr << "Viewer running. Press Q or close window to quit." << std::endl;

    // Main render loop
    bool quit = false;
    while (!quit) {
        // Handle SDL events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_ESCAPE) {
                    quit = true;
                }
            }
        }

        // Clear canvas to black
        memset(canvas.data(), 0, canvas.size());

        // Render each panel's pixels as LED blocks
        for (auto& panel : panels) {
            std::lock_guard<std::mutex> lock(panel->frameMutex);
            if (!panel->hasFrame) continue;

            for (int y = 0; y < panel->src_h; y++) {
                for (int x = 0; x < panel->src_w; x++) {
                    int srcIdx = (y * panel->src_w + x) * 3;
                    renderPixel(
                        panel->src_x + x, panel->src_y + y,
                        panel->frameBuffer[srcIdx],
                        panel->frameBuffer[srcIdx + 1],
                        panel->frameBuffer[srcIdx + 2]);
                }
            }
        }

        // Update texture and render (canvas is already at final resolution)
        SDL_UpdateTexture(texture, nullptr, canvas.data(), scaledW * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        // ~60 FPS render rate
        SDL_Delay(16);
    }

    // Cleanup
    running = false;
    for (auto& t : listenerThreads) {
        if (t.joinable()) t.join();
    }
    for (auto& panel : panels) {
        if (panel->udpSocket >= 0) close(panel->udpSocket);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
