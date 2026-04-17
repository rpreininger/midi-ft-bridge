// ====================================================================
//  Flaschen-Taschen UDP Sender Implementation
//  Sends frames as row-strip tiles to stay under MTU (no IP fragmentation)
// ====================================================================

#include "ft_sender.h"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

// Max rows per tile to keep UDP packets under ~1400 bytes
// 128 pixels * 3 bytes * 3 rows = 1152 bytes + ~30 byte header = ~1182 bytes
static const int TILE_ROWS = 3;

FTSender::FTSender()
    : m_socket(-1)
    , m_destAddr{}
    , m_hasAddr(false)
    , m_enabled(false)
    , m_framesSent(0)
    , m_bytesSent(0)
    , m_port(1337)
{
}

FTSender::~FTSender() {
    if (m_socket >= 0) {
        close(m_socket);
    }
}

bool FTSender::init(const std::string& host, int port) {
    m_host = host;
    m_port = port;

    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        std::cerr << "FTSender: Failed to create UDP socket" << std::endl;
        return false;
    }

    // Non-blocking so sendto never stalls the main loop
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);

    // Increase send buffer to reduce dropped packets
    int sndbuf = 256 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        std::cerr << "FTSender: Failed to resolve host: " << host << std::endl;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    memset(&m_destAddr, 0, sizeof(m_destAddr));
    m_destAddr.sin_family = AF_INET;
    m_destAddr.sin_port = htons(port);
    memcpy(&m_destAddr.sin_addr, he->h_addr_list[0], he->h_length);
    m_hasAddr = true;

    m_enabled.store(true);

    std::cerr << "FTSender: Initialized, sending to " << host << ":" << port
              << " (tile mode: " << TILE_ROWS << " rows/packet)" << std::endl;
    return true;
}

void FTSender::send(const uint8_t* framebuffer, int width, int height,
                    int offsetX, int offsetY) {
    if (!m_enabled.load() || m_socket < 0 || !m_hasAddr) {
        return;
    }

    // Send frame as row-strip tiles to avoid IP fragmentation
    for (int y = 0; y < height; y += TILE_ROWS) {
        int tileH = std::min(TILE_ROWS, height - y);
        int tilePixels = width * tileH * 3;

        // PPM header with FT offset
        char header[64];
        int headerLen = snprintf(header, sizeof(header),
                                "P6\n#FT: %d %d\n%d %d\n255\n",
                                offsetX, offsetY + y, width, tileH);

        int packetSize = headerLen + tilePixels;

        if ((int)m_packetBuffer.size() < packetSize) {
            m_packetBuffer.resize(packetSize);
        }

        memcpy(m_packetBuffer.data(), header, headerLen);
        memcpy(m_packetBuffer.data() + headerLen,
               framebuffer + (y * width * 3), tilePixels);

        sendto(m_socket, m_packetBuffer.data(), packetSize, 0,
               (struct sockaddr*)&m_destAddr, sizeof(m_destAddr));
    }

    m_framesSent++;
    m_bytesSent += width * height * 3;
}

void FTSender::sendRaw(const uint8_t* framebuffer, int width, int height) {
    if (!m_enabled.load() || m_socket < 0 || !m_hasAddr) {
        return;
    }

    // 4-byte header: uint16 LE width + uint16 LE height, then RGB24 data
    int rgbSize = width * height * 3;
    int packetSize = 4 + rgbSize;

    if ((int)m_packetBuffer.size() < packetSize) {
        m_packetBuffer.resize(packetSize);
    }

    // Little-endian width and height
    m_packetBuffer[0] = width & 0xFF;
    m_packetBuffer[1] = (width >> 8) & 0xFF;
    m_packetBuffer[2] = height & 0xFF;
    m_packetBuffer[3] = (height >> 8) & 0xFF;
    memcpy(m_packetBuffer.data() + 4, framebuffer, rgbSize);

    sendto(m_socket, m_packetBuffer.data(), packetSize, 0,
           (struct sockaddr*)&m_destAddr, sizeof(m_destAddr));

    m_framesSent++;
    m_bytesSent += packetSize;
}

void FTSender::sendBlack(int width, int height) {
    // Use a separate buffer — send() writes into m_packetBuffer,
    // so we can't use it as both source and destination.
    std::vector<uint8_t> black(width * height * 3, 0);
    send(black.data(), width, height);
}
