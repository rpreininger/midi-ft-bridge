// ====================================================================
//  Flaschen-Taschen UDP Sender
//  Sends frame data to a remote FT server using PPM P6 format over UDP
//  (Copied from raspi project, with pre-allocated packet buffer)
// ====================================================================

#ifndef FT_SENDER_H
#define FT_SENDER_H

#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

class FTSender {
public:
    FTSender();
    ~FTSender();

    // Initialize the sender with destination host and port
    bool init(const std::string& host, int port = 1337);

    // Send a frame to the FT server
    // framebuffer: RGB pixel data (width * height * 3 bytes)
    void send(const uint8_t* framebuffer, int width, int height,
              int offsetX = 0, int offsetY = 0);

    // Send a black frame (clear the panel)
    void sendBlack(int width, int height);

    // Check if sender is enabled and initialized
    bool isEnabled() const { return m_enabled.load(); }

    // Enable/disable sending
    void setEnabled(bool enabled) { m_enabled.store(enabled); }

    // Get statistics
    uint64_t getFramesSent() const { return m_framesSent.load(); }
    uint64_t getBytesSent() const { return m_bytesSent.load(); }

    // Get connection info
    const std::string& getHost() const { return m_host; }
    int getPort() const { return m_port; }

private:
    int m_socket;
    struct sockaddr_in* m_destAddr;
    std::atomic<bool> m_enabled;
    std::atomic<uint64_t> m_framesSent;
    std::atomic<uint64_t> m_bytesSent;
    std::string m_host;
    int m_port;

    // Pre-allocated packet buffer (avoids new/delete per frame)
    std::vector<uint8_t> m_packetBuffer;
};

#endif // FT_SENDER_H
