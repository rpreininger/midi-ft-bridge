// ====================================================================
//  BLE Sender - Stub implementation for macOS
//  Accepts frames but does nothing (no BlueZ/D-Bus on macOS).
// ====================================================================

#include "ble_sender.h"
#include <iostream>

BleSender::BleSender()
    : m_debug(false)
    , m_brightness(80)
    , m_dbus(nullptr)
    , m_ackReceived(false)
    , m_pendingWidth(0)
    , m_pendingHeight(0)
    , m_hasFrame(false)
    , m_running(false)
    , m_connected(false)
    , m_framesSent(0)
    , m_framesDropped(0)
{
}

BleSender::~BleSender() {
    stop();
}

bool BleSender::init(const std::string& addr, int brightness, bool debug) {
    m_addr = addr;
    m_brightness = brightness;
    m_debug = debug;
    std::cerr << "BleSender: Stub mode (no BlueZ). BLE panel " << addr << " disabled." << std::endl;
    return true;
}

void BleSender::sendFrame(const uint8_t* rgb, int width, int height) {
    (void)rgb; (void)width; (void)height;
    m_framesDropped++;
}

void BleSender::sendBlack(int width, int height) {
    (void)width; (void)height;
}

void BleSender::stop() {
    m_running = false;
}

void BleSender::workerThread() {}
bool BleSender::dbusConnect() { return false; }
void BleSender::dbusDisconnect() {}
bool BleSender::bleConnect() { return false; }
bool BleSender::bleWriteChar(const std::string& charPath, const uint8_t* data, size_t len) {
    (void)charPath; (void)data; (void)len; return false;
}
bool BleSender::bleStartNotify(const std::string& charPath) { (void)charPath; return false; }
std::string BleSender::findCharacteristic(const std::string& uuid) { (void)uuid; return ""; }
bool BleSender::waitForServicesResolved(int timeoutMs) { (void)timeoutMs; return false; }
bool BleSender::sendPng(const std::vector<uint8_t>& pngBytes) { (void)pngBytes; return false; }
bool BleSender::setBrightness(int brightness) { (void)brightness; return false; }
std::vector<uint8_t> BleSender::encodePng(const uint8_t* rgb, int width, int height) {
    (void)rgb; (void)width; (void)height; return {};
}
std::string BleSender::devicePath() const { return ""; }
