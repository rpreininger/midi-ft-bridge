// ====================================================================
//  BLE Sender - Direct BLE communication with iPixel Color panels
//  Replaces bt_bridge.py: encodes RGB->PNG and sends via BlueZ D-Bus
// ====================================================================

#ifndef BLE_SENDER_H
#define BLE_SENDER_H

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

class BleSender {
public:
    BleSender();
    ~BleSender();

    // Initialize with BLE device MAC address and brightness (0-100)
    bool init(const std::string& addr, int brightness = 80, bool debug = false);

    // Send a frame (RGB24, width*height*3 bytes). Non-blocking: queues the
    // latest frame and drops older ones if BLE is slower than input.
    void sendFrame(const uint8_t* rgb, int width, int height);

    // Send a black frame
    void sendBlack(int width, int height);

    // Stop the background thread and disconnect
    void stop();

    bool isConnected() const { return m_connected.load(); }

    // Statistics
    uint64_t getFramesSent() const { return m_framesSent.load(); }
    uint64_t getFramesDropped() const { return m_framesDropped.load(); }

    const std::string& getAddr() const { return m_addr; }

private:
    // Background thread: connect, send frames, reconnect
    void workerThread();

    // BlueZ D-Bus operations
    bool dbusConnect();
    void dbusDisconnect();
    bool bleConnect();
    bool bleWriteChar(const std::string& charPath, const uint8_t* data, size_t len);
    bool bleStartNotify(const std::string& charPath);
    std::string findCharacteristic(const std::string& uuid);
    bool waitForServicesResolved(int timeoutMs = 15000);

    // iPixel protocol
    bool sendPng(const std::vector<uint8_t>& pngBytes);
    bool setBrightness(int brightness);

    // PNG encoding (uses lodepng)
    static std::vector<uint8_t> encodePng(const uint8_t* rgb, int width, int height);

    // D-Bus helpers
    std::string devicePath() const;

    bool m_debug;
    std::string m_addr;         // BLE MAC address
    std::string m_addrDbus;     // MAC with underscores for D-Bus path
    int m_brightness;

    // D-Bus connection
    void* m_dbus;               // DBusConnection* (opaque to avoid header leak)

    // GATT characteristic paths (resolved after connect)
    std::string m_writeCharPath;
    std::string m_notifyCharPath;

    // ACK synchronization
    std::mutex m_ackMutex;
    std::condition_variable m_ackCv;
    bool m_ackReceived;

    // Frame queue (latest frame only)
    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    std::vector<uint8_t> m_pendingFrame;
    int m_pendingWidth;
    int m_pendingHeight;
    bool m_hasFrame;

    // Worker thread
    std::thread m_worker;
    std::atomic<bool> m_running;
    std::atomic<bool> m_connected;
    std::atomic<uint64_t> m_framesSent;
    std::atomic<uint64_t> m_framesDropped;
};

#endif // BLE_SENDER_H
