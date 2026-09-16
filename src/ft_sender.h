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
#include <netinet/in.h>

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

    // Send raw RGB frame with uint16 LE width+height header (for BLE bridge)
    void sendRaw(const uint8_t* framebuffer, int width, int height);

    // Send a black frame (clear the panel)
    void sendBlack(int width, int height);

    // Check if sender is enabled and initialized
    bool isEnabled() const { return m_enabled.load(); }

    // Enable/disable sending
    void setEnabled(bool enabled) { m_enabled.store(enabled); }

    // Get statistics. NOTE: framesSent counts frames handed to the socket,
    // not frames that arrived -- UDP gives us no delivery signal. A frame is
    // counted even if every one of its packets was refused locally, so read it
    // together with getSendErrors().
    uint64_t getFramesSent() const { return m_framesSent.load(); }
    uint64_t getBytesSent() const { return m_bytesSent.load(); }

    // Packets sendto() refused. Non-zero means the frames above went nowhere.
    // Typically EHOSTUNREACH/ENETUNREACH (this device is not on the panel's
    // subnet) or EPERM (iOS Local Network permission not granted); EAGAIN just
    // means the send buffer was momentarily full, which is congestion, not a
    // fault -- getLastErrorText() is what tells the two apart.
    uint64_t getSendErrors() const { return m_sendErrors.load(); }
    std::string getLastErrorText() const;

    // Get connection info
    const std::string& getHost() const { return m_host; }
    int getPort() const { return m_port; }

private:
    int m_socket;
    struct sockaddr_in m_destAddr;
    bool m_hasAddr;
    std::atomic<bool> m_enabled;
    std::atomic<uint64_t> m_framesSent;
    std::atomic<uint64_t> m_bytesSent;
    std::atomic<uint64_t> m_sendErrors;
    std::atomic<int> m_lastErrno;
    std::string m_host;
    int m_port;

    // Pre-allocated packet buffer (avoids new/delete per frame)
    std::vector<uint8_t> m_packetBuffer;

    // Record `failedPackets` refused sends (errno `err`) and log, throttled.
    void noteSendError(uint64_t failedPackets, int err);
};

#endif // FT_SENDER_H
