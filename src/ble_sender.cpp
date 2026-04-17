// ====================================================================
//  BLE Sender Implementation
//  Uses BlueZ D-Bus API for GATT BLE communication with iPixel panels
// ====================================================================

#include "ble_sender.h"
#include "lodepng.h"

#include <iostream>
#include <cstring>
#include <algorithm>
#include <chrono>

#include <dbus/dbus.h>

// iPixel BLE protocol constants
static const char* WRITE_UUID  = "0000fa02-0000-1000-8000-00805f9b34fb";
static const char* NOTIFY_UUID = "0000fa03-0000-1000-8000-00805f9b34fb";
static const int   CHUNK_SIZE  = 244;
static const int   WINDOW_SIZE = 12 * 1024;
static const int   ACK_TIMEOUT_MS = 3000;

// BlueZ D-Bus constants
static const char* BLUEZ_SERVICE = "org.bluez";
static const char* ADAPTER_PATH  = "/org/bluez/hci0";
static const char* DBUS_OM_IFACE = "org.freedesktop.DBus.ObjectManager";
static const char* DBUS_PROP_IFACE = "org.freedesktop.DBus.Properties";
static const char* BLUEZ_DEVICE_IFACE = "org.bluez.Device1";
static const char* BLUEZ_CHAR_IFACE   = "org.bluez.GattCharacteristic1";

// CRC32 (same table as Python's binascii.crc32)
static const uint32_t* crc32_table() {
    static uint32_t table[256] = {};
    static bool init = [&]() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    return table;
}

static uint32_t crc32(const uint8_t* data, size_t len) {
    const uint32_t* table = crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

// ---- D-Bus helper: call a method and return the reply ----
static DBusMessage* dbusCall(DBusConnection* conn,
                             const char* dest, const char* path,
                             const char* iface, const char* method,
                             int firstArgType = DBUS_TYPE_INVALID, ...) {
    DBusMessage* msg = dbus_message_new_method_call(dest, path, iface, method);
    if (!msg) return nullptr;

    if (firstArgType != DBUS_TYPE_INVALID) {
        va_list args;
        va_start(args, firstArgType);
        dbus_message_append_args_valist(msg, firstArgType, args);
        va_end(args);
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 15000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        // Don't spam for expected errors during connection
        if (strstr(err.name, "NoReply") == nullptr &&
            strstr(err.name, "ServiceUnknown") == nullptr) {
            std::cerr << "BLE D-Bus: " << err.name << ": " << err.message << std::endl;
        }
        dbus_error_free(&err);
        return nullptr;
    }
    return reply;
}

// ---- D-Bus helper: get a string property ----
static std::string dbusGetStringProp(DBusConnection* conn,
                                     const char* path, const char* iface,
                                     const char* prop) {
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, path, DBUS_PROP_IFACE, "Get");
    if (!msg) return "";

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return "";
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&variant, &val);
            std::string result(val ? val : "");
            dbus_message_unref(reply);
            return result;
        }
    }
    dbus_message_unref(reply);
    return "";
}

// ---- D-Bus helper: get a boolean property ----
static bool dbusGetBoolProp(DBusConnection* conn,
                            const char* path, const char* iface,
                            const char* prop) {
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, path, DBUS_PROP_IFACE, "Get");
    if (!msg) return false;

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t val = FALSE;
            dbus_message_iter_get_basic(&variant, &val);
            dbus_message_unref(reply);
            return val != FALSE;
        }
    }
    dbus_message_unref(reply);
    return false;
}

// ==================================================================

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

    // Convert MAC "D2:DF:25:F1:E1:3D" -> "D2_DF_25_F1_E1_3D" for D-Bus path
    m_addrDbus = addr;
    std::replace(m_addrDbus.begin(), m_addrDbus.end(), ':', '_');

    m_running = true;
    m_worker = std::thread(&BleSender::workerThread, this);

    std::cerr << "BleSender: Initialized for " << addr
              << ", brightness=" << brightness << std::endl;
    return true;
}

void BleSender::sendFrame(const uint8_t* rgb, int width, int height) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_hasFrame) {
        m_framesDropped++;
    }
    int size = width * height * 3;
    m_pendingFrame.assign(rgb, rgb + size);
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_hasFrame = true;
    m_frameCv.notify_one();
}

void BleSender::sendBlack(int width, int height) {
    std::vector<uint8_t> black(width * height * 3, 0);
    sendFrame(black.data(), width, height);
}

void BleSender::stop() {
    m_running = false;
    m_frameCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    dbusDisconnect();
}

std::string BleSender::devicePath() const {
    return std::string(ADAPTER_PATH) + "/dev_" + m_addrDbus;
}

// ---- Background worker thread ----
void BleSender::workerThread() {
    while (m_running) {
        // Connect
        if (!m_connected) {
            if (!dbusConnect()) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            if (!bleConnect()) {
                dbusDisconnect();
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            // Resolve GATT characteristics
            m_writeCharPath = findCharacteristic(WRITE_UUID);
            m_notifyCharPath = findCharacteristic(NOTIFY_UUID);
            if (m_writeCharPath.empty() || m_notifyCharPath.empty()) {
                std::cerr << "BleSender: Failed to find GATT characteristics" << std::endl;
                dbusDisconnect();
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }

            // Start notifications
            bleStartNotify(m_notifyCharPath);

            // Set brightness
            setBrightness(m_brightness);

            // Clear any cached image on the panel
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                int w = m_pendingWidth > 0 ? m_pendingWidth : 32;
                int h = m_pendingHeight > 0 ? m_pendingHeight : 16;
                std::vector<uint8_t> black(w * h * 3, 0);
                auto blackPng = encodePng(black.data(), w, h);
                if (!blackPng.empty()) sendPng(blackPng);
            }

            m_connected = true;
            std::cerr << "BleSender: Connected to " << m_addr << std::endl;
        }

        // Wait for a frame
        std::vector<uint8_t> frameData;
        int width, height;
        {
            std::unique_lock<std::mutex> lock(m_frameMutex);
            m_frameCv.wait_for(lock, std::chrono::milliseconds(100),
                               [this] { return m_hasFrame || !m_running; });
            if (!m_running) break;
            if (!m_hasFrame) continue;

            frameData = std::move(m_pendingFrame);
            width = m_pendingWidth;
            height = m_pendingHeight;
            m_hasFrame = false;
        }

        // Encode RGB -> PNG
        auto pngBytes = encodePng(frameData.data(), width, height);
        if (pngBytes.empty()) continue;

        // Send via BLE protocol
        auto sendStart = std::chrono::steady_clock::now();
        if (!sendPng(pngBytes)) {
            std::cerr << "BleSender: Send failed, reconnecting..." << std::endl;
            m_connected = false;
            dbusDisconnect();
            continue;
        }
        auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - sendStart).count();

        m_framesSent++;
        if (m_debug) {
            std::cerr << "BLE-DBG: frame " << m_framesSent.load()
                      << " PNG=" << pngBytes.size() << "B"
                      << " send=" << sendMs << "ms"
                      << " dropped=" << m_framesDropped.load() << std::endl;
        }
    }
}

// ---- D-Bus connection ----
bool BleSender::dbusConnect() {
    DBusError err;
    dbus_error_init(&err);

    auto* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        std::cerr << "BleSender: D-Bus connect failed: "
                  << (dbus_error_is_set(&err) ? err.message : "unknown") << std::endl;
        dbus_error_free(&err);
        return false;
    }

    m_dbus = conn;
    return true;
}

void BleSender::dbusDisconnect() {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    if (conn) {
        // Disconnect BLE device
        auto* reply = dbusCall(conn, BLUEZ_SERVICE, devicePath().c_str(),
                               BLUEZ_DEVICE_IFACE, "Disconnect");
        if (reply) dbus_message_unref(reply);

        dbus_connection_unref(conn);
        m_dbus = nullptr;
    }
    m_connected = false;
}

// ---- BLE Connect via BlueZ ----
bool BleSender::bleConnect() {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    if (!conn) return false;

    std::string devPath = devicePath();
    std::cerr << "BleSender: Connecting to " << m_addr << "..." << std::endl;

    // Check if already connected
    if (dbusGetBoolProp(conn, devPath.c_str(), BLUEZ_DEVICE_IFACE, "Connected")) {
        std::cerr << "BleSender: Already connected" << std::endl;
    } else {
        // Call Device1.Connect()
        auto* reply = dbusCall(conn, BLUEZ_SERVICE, devPath.c_str(),
                               BLUEZ_DEVICE_IFACE, "Connect");
        if (!reply) {
            std::cerr << "BleSender: Connect call failed" << std::endl;
            return false;
        }
        dbus_message_unref(reply);
    }

    // Wait for GATT services to be resolved
    if (!waitForServicesResolved()) {
        std::cerr << "BleSender: Services not resolved (timeout)" << std::endl;
        return false;
    }

    return true;
}

bool BleSender::waitForServicesResolved(int timeoutMs) {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    std::string devPath = devicePath();

    auto start = std::chrono::steady_clock::now();
    while (m_running) {
        if (dbusGetBoolProp(conn, devPath.c_str(), BLUEZ_DEVICE_IFACE, "ServicesResolved")) {
            return true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// ---- Find GATT characteristic by UUID ----
std::string BleSender::findCharacteristic(const std::string& uuid) {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    if (!conn) return "";

    // Get all managed objects from BlueZ
    auto* reply = dbusCall(conn, BLUEZ_SERVICE, "/",
                           DBUS_OM_IFACE, "GetManagedObjects");
    if (!reply) return "";

    std::string result;
    std::string devPath = devicePath();

    DBusMessageIter rootIter;
    dbus_message_iter_init(reply, &rootIter);

    // The reply is a dict: object_path -> dict of interfaces
    if (dbus_message_iter_get_arg_type(&rootIter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return "";
    }

    DBusMessageIter arrayIter;
    dbus_message_iter_recurse(&rootIter, &arrayIter);

    while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dictEntry;
        dbus_message_iter_recurse(&arrayIter, &dictEntry);

        // Get object path
        const char* objPath = nullptr;
        dbus_message_iter_get_basic(&dictEntry, &objPath);

        // Only look at paths under our device
        if (objPath && strstr(objPath, devPath.c_str()) == objPath) {
            // Check if this object has GattCharacteristic1 interface
            dbus_message_iter_next(&dictEntry);
            DBusMessageIter ifacesIter;
            dbus_message_iter_recurse(&dictEntry, &ifacesIter);

            while (dbus_message_iter_get_arg_type(&ifacesIter) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ifaceEntry;
                dbus_message_iter_recurse(&ifacesIter, &ifaceEntry);

                const char* ifaceName = nullptr;
                dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);

                if (ifaceName && strcmp(ifaceName, BLUEZ_CHAR_IFACE) == 0) {
                    // Check UUID property in the properties dict
                    dbus_message_iter_next(&ifaceEntry);
                    DBusMessageIter propsIter;
                    dbus_message_iter_recurse(&ifaceEntry, &propsIter);

                    while (dbus_message_iter_get_arg_type(&propsIter) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter propEntry;
                        dbus_message_iter_recurse(&propsIter, &propEntry);

                        const char* propName = nullptr;
                        dbus_message_iter_get_basic(&propEntry, &propName);

                        if (propName && strcmp(propName, "UUID") == 0) {
                            dbus_message_iter_next(&propEntry);
                            DBusMessageIter variantIter;
                            dbus_message_iter_recurse(&propEntry, &variantIter);

                            if (dbus_message_iter_get_arg_type(&variantIter) == DBUS_TYPE_STRING) {
                                const char* charUuid = nullptr;
                                dbus_message_iter_get_basic(&variantIter, &charUuid);
                                if (charUuid && uuid == charUuid) {
                                    result = objPath;
                                }
                            }
                        }
                        dbus_message_iter_next(&propsIter);
                    }
                }
                dbus_message_iter_next(&ifacesIter);
            }
        }
        dbus_message_iter_next(&arrayIter);

        if (!result.empty()) break;
    }

    dbus_message_unref(reply);
    return result;
}

// ---- Write to a GATT characteristic ----
bool BleSender::bleWriteChar(const std::string& charPath,
                             const uint8_t* data, size_t len) {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    if (!conn || charPath.empty()) return false;

    // Call GattCharacteristic1.WriteValue(data, options{})
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, charPath.c_str(), BLUEZ_CHAR_IFACE, "WriteValue");
    if (!msg) return false;

    DBusMessageIter iter, arrayIter, dictIter;
    dbus_message_iter_init_append(msg, &iter);

    // Append byte array
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &arrayIter);
    for (size_t i = 0; i < len; i++) {
        dbus_message_iter_append_basic(&arrayIter, DBUS_TYPE_BYTE, &data[i]);
    }
    dbus_message_iter_close_container(&iter, &arrayIter);

    // Append empty options dict a{sv} (write-with-response, as required by iPixel)
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dictIter);
    dbus_message_iter_close_container(&iter, &dictIter);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        std::cerr << "BleSender: WriteValue failed: " << err.message << std::endl;
        dbus_error_free(&err);
        return false;
    }
    if (reply) dbus_message_unref(reply);
    return true;
}

// ---- Start notifications on a characteristic ----
bool BleSender::bleStartNotify(const std::string& charPath) {
    auto* conn = static_cast<DBusConnection*>(m_dbus);
    if (!conn || charPath.empty()) return false;

    // Add a match rule to receive PropertiesChanged signals for this char
    std::string rule = "type='signal',sender='org.bluez',interface='"
        + std::string(DBUS_PROP_IFACE) + "',member='PropertiesChanged',path='"
        + charPath + "'";
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, rule.c_str(), &err);
    if (dbus_error_is_set(&err)) {
        std::cerr << "BleSender: add_match failed: " << err.message << std::endl;
        dbus_error_free(&err);
    }

    auto* reply = dbusCall(conn, BLUEZ_SERVICE, charPath.c_str(),
                           BLUEZ_CHAR_IFACE, "StartNotify");
    if (reply) {
        dbus_message_unref(reply);
        return true;
    }
    return false;
}

// ---- Check for ACK notification via D-Bus signals ----
static bool checkForAck(DBusConnection* conn, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        // Process pending D-Bus messages
        dbus_connection_read_write(conn, 100);

        DBusMessage* sig = dbus_connection_pop_message(conn);
        while (sig) {
            if (dbus_message_is_signal(sig, DBUS_PROP_IFACE, "PropertiesChanged")) {
                // Parse the PropertiesChanged signal to look for Value changes
                DBusMessageIter iter;
                dbus_message_iter_init(sig, &iter);

                // Skip interface name
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                    dbus_message_iter_next(&iter);

                    // Changed properties dict
                    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
                        DBusMessageIter dictIter;
                        dbus_message_iter_recurse(&iter, &dictIter);

                        while (dbus_message_iter_get_arg_type(&dictIter) == DBUS_TYPE_DICT_ENTRY) {
                            DBusMessageIter entry;
                            dbus_message_iter_recurse(&dictIter, &entry);

                            const char* propName = nullptr;
                            dbus_message_iter_get_basic(&entry, &propName);

                            if (propName && strcmp(propName, "Value") == 0) {
                                dbus_message_iter_next(&entry);
                                DBusMessageIter variant, arrIter;
                                dbus_message_iter_recurse(&entry, &variant);

                                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
                                    dbus_message_iter_recurse(&variant, &arrIter);

                                    // Read first byte — ACK is [0x05, ...]
                                    uint8_t firstByte = 0;
                                    int count = 0;
                                    DBusMessageIter countIter = arrIter;
                                    while (dbus_message_iter_get_arg_type(&countIter) != DBUS_TYPE_INVALID) {
                                        count++;
                                        dbus_message_iter_next(&countIter);
                                    }

                                    if (count == 5) {
                                        dbus_message_iter_get_basic(&arrIter, &firstByte);
                                        if (firstByte == 0x05) {
                                            dbus_message_unref(sig);
                                            return true;
                                        }
                                    }
                                }
                            }
                            dbus_message_iter_next(&dictIter);
                        }
                    }
                }
            }
            dbus_message_unref(sig);
            sig = dbus_connection_pop_message(conn);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) return false;
    }
}

// ---- iPixel protocol: send PNG image ----
bool BleSender::sendPng(const std::vector<uint8_t>& pngBytes) {
    uint32_t crc = crc32(pngBytes.data(), pngBytes.size());
    uint32_t totalLen = (uint32_t)pngBytes.size();
    auto* conn = static_cast<DBusConnection*>(m_dbus);

    size_t offset = 0;
    bool first = true;

    while (offset < pngBytes.size()) {
        size_t windowDataLen = std::min((size_t)WINDOW_SIZE, pngBytes.size() - offset);

        // Build window header (13 bytes)
        uint8_t option = first ? 0x00 : 0x02;
        first = false;

        std::vector<uint8_t> windowData;
        // Header: type, 0, option, totalLen(4 LE), crc(4 LE), format, slot
        windowData.push_back(0x02);  // PNG type
        windowData.push_back(0x00);
        windowData.push_back(option);
        windowData.push_back(totalLen & 0xFF);
        windowData.push_back((totalLen >> 8) & 0xFF);
        windowData.push_back((totalLen >> 16) & 0xFF);
        windowData.push_back((totalLen >> 24) & 0xFF);
        windowData.push_back(crc & 0xFF);
        windowData.push_back((crc >> 8) & 0xFF);
        windowData.push_back((crc >> 16) & 0xFF);
        windowData.push_back((crc >> 24) & 0xFF);
        windowData.push_back(0x00);  // PNG format
        windowData.push_back(0x00);  // save slot 0

        // Append PNG data for this window
        windowData.insert(windowData.end(),
                          pngBytes.begin() + offset,
                          pngBytes.begin() + offset + windowDataLen);

        // Prepend length (2 bytes LE) — total length including these 2 bytes
        uint16_t length = (uint16_t)(windowData.size() + 2);
        std::vector<uint8_t> fullWindow;
        fullWindow.push_back(length & 0xFF);
        fullWindow.push_back((length >> 8) & 0xFF);
        fullWindow.insert(fullWindow.end(), windowData.begin(), windowData.end());

        // Send in CHUNK_SIZE chunks
        for (size_t i = 0; i < fullWindow.size(); i += CHUNK_SIZE) {
            size_t chunkLen = std::min((size_t)CHUNK_SIZE, fullWindow.size() - i);
            if (!bleWriteChar(m_writeCharPath, fullWindow.data() + i, chunkLen)) {
                return false;
            }
        }

        // Wait for ACK
        if (!checkForAck(conn, ACK_TIMEOUT_MS)) {
            if (m_debug) std::cerr << "BLE-DBG: ACK TIMEOUT" << std::endl;
        }

        offset += windowDataLen;
    }

    return true;
}

// ---- Set panel brightness ----
bool BleSender::setBrightness(int brightness) {
    uint8_t cmd[] = { 5, 0, 4, 0x80, (uint8_t)brightness };
    return bleWriteChar(m_writeCharPath, cmd, sizeof(cmd));
}

// ---- Encode RGB24 to PNG using lodepng ----
std::vector<uint8_t> BleSender::encodePng(const uint8_t* rgb, int width, int height) {
    std::vector<uint8_t> png;
    unsigned error = lodepng::encode(png, rgb, (unsigned)width, (unsigned)height, LCT_RGB, 8);
    if (error) {
        std::cerr << "BleSender: PNG encode error: " << lodepng_error_text(error) << std::endl;
        return {};
    }
    return png;
}
