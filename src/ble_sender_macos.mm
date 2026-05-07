// ====================================================================
//  BLE Sender - CoreBluetooth implementation for macOS
//
//  Replaces the BlueZ/D-Bus version on Linux. iPixel protocol (chunked
//  PNG over GATT writes with ACK notifications) is identical; only the
//  transport layer differs.
//
//  Threading model:
//    - All CoreBluetooth work runs on a private serial dispatch queue
//      (CB requires a queue you give it; delegate callbacks fire there).
//    - The C++ worker thread (BleSender::workerThread) is the same as
//      the Linux version — it calls into the Obj-C client synchronously
//      via dispatch_semaphore_t for connect/write/ack.
// ====================================================================

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>

#include "ble_sender.h"
#include "lodepng.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

// iPixel protocol constants (same as Linux side)
static NSString * const kWriteUUID  = @"0000fa02-0000-1000-8000-00805f9b34fb";
static NSString * const kNotifyUUID = @"0000fa03-0000-1000-8000-00805f9b34fb";
static const int kChunkSize    = 244;
static const int kWindowSize   = 12 * 1024;
static const int kAckTimeoutMs = 3000;

// ---------- CRC32 (matches Python binascii.crc32 / Linux side) -----------
static uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256] = {};
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        initialized = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

// =====================================================================
//  Obj-C client: owns the CBCentralManager + peripheral and handles all
//  delegate callbacks. Exposes synchronous primitives to the C++ side.
// =====================================================================
@interface MFBIPixelClient : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (atomic, assign) BOOL ready;     // characteristics resolved + notifications on
@property (atomic, assign) BOOL debug;
@end

@implementation MFBIPixelClient {
    dispatch_queue_t  _queue;
    CBCentralManager *_central;
    CBPeripheral     *_peripheral;
    CBCharacteristic *_writeChar;
    CBCharacteristic *_notifyChar;

    NSString         *_namePattern;     // case-insensitive substring match
    BOOL              _scanning;

    // Connection lifecycle: signaled when the chain (poweredOn -> scan ->
    // connect -> discover -> notify-on) completes or fails.
    dispatch_semaphore_t _readySema;
    BOOL                 _readyResult;

    // Per-write completion (didWriteValueForCharacteristic:).
    dispatch_semaphore_t _writeSema;
    BOOL                 _writeOk;

    // ACK signaling (didUpdateValueForCharacteristic: on notify char).
    dispatch_semaphore_t _ackSema;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _queue = dispatch_queue_create("de.welt.midiftbridge.ble", DISPATCH_QUEUE_SERIAL);
        _readySema = dispatch_semaphore_create(0);
        _writeSema = dispatch_semaphore_create(0);
        _ackSema   = dispatch_semaphore_create(0);
    }
    return self;
}

- (BOOL)connectMatchingName:(NSString *)namePattern timeout:(NSTimeInterval)timeoutSec {
    _namePattern = [namePattern copy] ?: @"";
    self.ready = NO;
    _readyResult = NO;

    // Drain any pending semaphore signal from previous attempts.
    while (dispatch_semaphore_wait(_readySema, DISPATCH_TIME_NOW) == 0) {}

    dispatch_async(_queue, ^{
        if (!self->_central) {
            self->_central = [[CBCentralManager alloc] initWithDelegate:self queue:self->_queue];
        } else if (self->_central.state == CBManagerStatePoweredOn) {
            [self startScan];
        }
        // Otherwise wait for centralManagerDidUpdateState: to fire when BT comes up.
    });

    long rc = dispatch_semaphore_wait(_readySema,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec * NSEC_PER_SEC)));
    if (rc != 0) {
        std::cerr << "BleSender(macOS): connect timed out after "
                  << timeoutSec << "s" << std::endl;
        dispatch_async(self->_queue, ^{ [self stopScan]; });
        return NO;
    }
    self.ready = _readyResult;
    return _readyResult;
}

- (BOOL)writeBytes:(const void *)bytes length:(NSUInteger)len {
    if (!self.ready || !_peripheral || !_writeChar) return NO;
    NSData *data = [NSData dataWithBytes:bytes length:len];

    // Drain stale signal.
    while (dispatch_semaphore_wait(_writeSema, DISPATCH_TIME_NOW) == 0) {}
    _writeOk = NO;

    dispatch_async(_queue, ^{
        if (self->_peripheral && self->_writeChar) {
            [self->_peripheral writeValue:data
                        forCharacteristic:self->_writeChar
                                     type:CBCharacteristicWriteWithResponse];
        } else {
            dispatch_semaphore_signal(self->_writeSema);
        }
    });

    long rc = dispatch_semaphore_wait(_writeSema,
        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return rc == 0 && _writeOk;
}

- (BOOL)waitForAckWithTimeout:(NSTimeInterval)timeoutSec {
    // Drain any stale ACK first — every wait should see only the next ACK.
    while (dispatch_semaphore_wait(_ackSema, DISPATCH_TIME_NOW) == 0) {}
    long rc = dispatch_semaphore_wait(_ackSema,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec * NSEC_PER_SEC)));
    return rc == 0;
}

- (void)disconnect {
    dispatch_sync(_queue, ^{
        [self stopScan];
        if (self->_peripheral) {
            [self->_central cancelPeripheralConnection:self->_peripheral];
        }
        self->_peripheral = nil;
        self->_writeChar  = nil;
        self->_notifyChar = nil;
        self.ready = NO;
    });
}

#pragma mark - Internal helpers (must run on _queue)

- (void)startScan {
    if (_scanning) return;
    _scanning = YES;
    if (self.debug) std::cerr << "BleSender(macOS): scanning..." << std::endl;
    [_central scanForPeripheralsWithServices:nil options:nil];
}

- (void)stopScan {
    if (!_scanning) return;
    _scanning = NO;
    [_central stopScan];
}

- (void)signalReady:(BOOL)ok {
    _readyResult = ok;
    dispatch_semaphore_signal(_readySema);
}

#pragma mark - CBCentralManagerDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    switch (central.state) {
        case CBManagerStatePoweredOn:
            if (self.debug) std::cerr << "BleSender(macOS): Bluetooth on" << std::endl;
            [self startScan];
            break;
        case CBManagerStatePoweredOff:
            std::cerr << "BleSender(macOS): Bluetooth is OFF" << std::endl;
            [self signalReady:NO];
            break;
        case CBManagerStateUnauthorized:
            std::cerr << "BleSender(macOS): Bluetooth permission denied (check Privacy & Security)" << std::endl;
            [self signalReady:NO];
            break;
        case CBManagerStateUnsupported:
            std::cerr << "BleSender(macOS): BLE unsupported on this Mac" << std::endl;
            [self signalReady:NO];
            break;
        default:
            break;
    }
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *,id> *)adv
                  RSSI:(NSNumber *)rssi {
    NSString *advName = adv[CBAdvertisementDataLocalNameKey] ?: peripheral.name ?: @"";
    if (_namePattern.length > 0) {
        // Case-insensitive substring match
        if ([advName rangeOfString:_namePattern
                           options:NSCaseInsensitiveSearch].location == NSNotFound) {
            return;
        }
    } else if (![advName.lowercaseString containsString:@"ipixel"]) {
        // No pattern given — best-effort: match any "iPixel*" device.
        return;
    }

    std::cerr << "BleSender(macOS): found "
              << [advName UTF8String] << " (RSSI=" << rssi.intValue << "), connecting..."
              << std::endl;

    [self stopScan];
    _peripheral = peripheral;
    _peripheral.delegate = self;
    [_central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager *)central
  didConnectPeripheral:(CBPeripheral *)peripheral {
    if (self.debug) std::cerr << "BleSender(macOS): connected, discovering services..." << std::endl;
    [peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)central
didFailToConnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    std::cerr << "BleSender(macOS): connect failed: "
              << [error.localizedDescription UTF8String] << std::endl;
    [self signalReady:NO];
}

- (void)centralManager:(CBCentralManager *)central
didDisconnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    std::cerr << "BleSender(macOS): disconnected"
              << (error ? [[NSString stringWithFormat:@" (%@)", error.localizedDescription] UTF8String] : "")
              << std::endl;
    self.ready = NO;
    _writeChar  = nil;
    _notifyChar = nil;
    _peripheral = nil;
    // No automatic resignal — the C++ worker drives reconnection.
}

#pragma mark - CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    if (error) {
        std::cerr << "BleSender(macOS): service discovery error: "
                  << [error.localizedDescription UTF8String] << std::endl;
        [self signalReady:NO];
        return;
    }
    for (CBService *svc in peripheral.services) {
        [peripheral discoverCharacteristics:nil forService:svc];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error {
    if (error) {
        std::cerr << "BleSender(macOS): characteristic discovery error: "
                  << [error.localizedDescription UTF8String] << std::endl;
        return;
    }
    for (CBCharacteristic *ch in service.characteristics) {
        NSString *uuid = ch.UUID.UUIDString.lowercaseString;
        // CB returns 16-bit UUIDs as "FA02"; iPixel uses 128-bit form. Match both.
        if ([uuid isEqualToString:kWriteUUID.lowercaseString] ||
            [uuid isEqualToString:@"fa02"]) {
            _writeChar = ch;
        } else if ([uuid isEqualToString:kNotifyUUID.lowercaseString] ||
                   [uuid isEqualToString:@"fa03"]) {
            _notifyChar = ch;
        }
    }
    if (_writeChar && _notifyChar) {
        [peripheral setNotifyValue:YES forCharacteristic:_notifyChar];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error {
    if (error) {
        std::cerr << "BleSender(macOS): setNotify failed: "
                  << [error.localizedDescription UTF8String] << std::endl;
        [self signalReady:NO];
        return;
    }
    if (characteristic == _notifyChar && characteristic.isNotifying) {
        if (self.debug) std::cerr << "BleSender(macOS): ready" << std::endl;
        [self signalReady:YES];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error {
    _writeOk = (error == nil);
    if (error) {
        std::cerr << "BleSender(macOS): write error: "
                  << [error.localizedDescription UTF8String] << std::endl;
    }
    dispatch_semaphore_signal(_writeSema);
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error {
    if (error || characteristic != _notifyChar) return;
    NSData *val = characteristic.value;
    if (val.length == 5) {
        const uint8_t *b = (const uint8_t *)val.bytes;
        if (b[0] == 0x05) dispatch_semaphore_signal(_ackSema);
    }
}

@end


// =====================================================================
//  C++ BleSender — wraps MFBIPixelClient and runs the same outer loop
//  as the Linux/BlueZ implementation (queue frame → encode PNG → window
//  → chunk → ACK).
//
//  m_dbus is repurposed to hold the Obj-C client (via bridged retain).
// =====================================================================

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

bool BleSender::init(const std::string& addr, int brightness, bool debug,
                     const std::string& name) {
    m_addr = addr;
    m_brightness = brightness;
    m_debug = debug;

    // CoreBluetooth has no MAC notion; we match by advertised local name.
    // Empty name means: client falls back to "any iPixel device".
    MFBIPixelClient *client = [[MFBIPixelClient alloc] init];
    client.debug = debug ? YES : NO;
    m_dbus = (__bridge_retained void *)client;
    m_addrDbus = name;  // pattern used by workerThread; empty = "any iPixel"

    m_running = true;
    m_worker = std::thread(&BleSender::workerThread, this);

    std::cerr << "BleSender(macOS): initialized for "
              << (name.empty() ? "(any iPixel)" : name)
              << ", brightness=" << brightness << std::endl;
    return true;
}

void BleSender::sendFrame(const uint8_t* rgb, int width, int height) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_hasFrame) m_framesDropped++;
    int size = width * height * 3;
    m_pendingFrame.assign(rgb, rgb + size);
    m_pendingWidth  = width;
    m_pendingHeight = height;
    m_hasFrame = true;
    m_frameCv.notify_one();
}

void BleSender::sendBlack(int width, int height) {
    std::vector<uint8_t> black(width * height * 3, 0);
    sendFrame(black.data(), width, height);
}

void BleSender::stop() {
    if (!m_running.exchange(false)) return;
    m_frameCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    dbusDisconnect();
}

void BleSender::dbusDisconnect() {
    if (!m_dbus) return;
    MFBIPixelClient *client = (__bridge_transfer MFBIPixelClient *)m_dbus;
    [client disconnect];
    client = nil;
    m_dbus = nullptr;
    m_connected = false;
}

void BleSender::workerThread() {
    MFBIPixelClient *client = (__bridge MFBIPixelClient *)m_dbus;
    NSString *pattern = m_addrDbus.empty() ? @""
        : [NSString stringWithUTF8String:m_addrDbus.c_str()];

    while (m_running) {
        if (!m_connected) {
            BOOL ok = [client connectMatchingName:pattern timeout:15.0];
            if (!ok) {
                if (!m_running) break;
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }

            // Initial brightness + clear screen (same dance as Linux side)
            setBrightness(m_brightness);
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                int w = m_pendingWidth > 0 ? m_pendingWidth : 32;
                int h = m_pendingHeight > 0 ? m_pendingHeight : 16;
                std::vector<uint8_t> black(w * h * 3, 0);
                auto blackPng = encodePng(black.data(), w, h);
                if (!blackPng.empty()) sendPng(blackPng);
            }

            m_connected = true;
            std::cerr << "BleSender(macOS): connected to "
                      << (m_addrDbus.empty() ? "(any iPixel)" : m_addrDbus)
                      << std::endl;
        }

        // Wait for a frame
        std::vector<uint8_t> frameData;
        int width = 0, height = 0;
        {
            std::unique_lock<std::mutex> lock(m_frameMutex);
            m_frameCv.wait_for(lock, std::chrono::milliseconds(100),
                               [this] { return m_hasFrame || !m_running; });
            if (!m_running) break;
            if (!m_hasFrame) continue;
            frameData = std::move(m_pendingFrame);
            width  = m_pendingWidth;
            height = m_pendingHeight;
            m_hasFrame = false;
        }

        auto pngBytes = encodePng(frameData.data(), width, height);
        if (pngBytes.empty()) continue;

        auto sendStart = std::chrono::steady_clock::now();
        if (!sendPng(pngBytes)) {
            std::cerr << "BleSender(macOS): send failed, reconnecting..." << std::endl;
            m_connected = false;
            [client disconnect];
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

bool BleSender::sendPng(const std::vector<uint8_t>& pngBytes) {
    MFBIPixelClient *client = (__bridge MFBIPixelClient *)m_dbus;
    if (!client) return false;

    uint32_t crc      = crc32(pngBytes.data(), pngBytes.size());
    uint32_t totalLen = (uint32_t)pngBytes.size();

    size_t offset = 0;
    bool first = true;

    while (offset < pngBytes.size()) {
        size_t windowDataLen = std::min((size_t)kWindowSize, pngBytes.size() - offset);
        uint8_t option = first ? 0x00 : 0x02;
        first = false;

        std::vector<uint8_t> windowData;
        windowData.reserve(13 + windowDataLen);
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
        windowData.insert(windowData.end(),
                          pngBytes.begin() + offset,
                          pngBytes.begin() + offset + windowDataLen);

        // 2-byte LE length prefix (total including itself)
        uint16_t length = (uint16_t)(windowData.size() + 2);
        std::vector<uint8_t> fullWindow;
        fullWindow.reserve(2 + windowData.size());
        fullWindow.push_back(length & 0xFF);
        fullWindow.push_back((length >> 8) & 0xFF);
        fullWindow.insert(fullWindow.end(), windowData.begin(), windowData.end());

        // Write in CHUNK_SIZE chunks, each with response.
        for (size_t i = 0; i < fullWindow.size(); i += kChunkSize) {
            size_t chunkLen = std::min((size_t)kChunkSize, fullWindow.size() - i);
            if (![client writeBytes:fullWindow.data() + i length:chunkLen]) {
                return false;
            }
        }

        // ACK is best-effort: log timeout but continue (matches Linux behavior).
        if (![client waitForAckWithTimeout:(kAckTimeoutMs / 1000.0)]) {
            if (m_debug) std::cerr << "BLE-DBG: ACK TIMEOUT" << std::endl;
        }

        offset += windowDataLen;
    }
    return true;
}

bool BleSender::setBrightness(int brightness) {
    MFBIPixelClient *client = (__bridge MFBIPixelClient *)m_dbus;
    if (!client) return false;
    uint8_t cmd[] = { 5, 0, 4, 0x80, (uint8_t)brightness };
    return [client writeBytes:cmd length:sizeof(cmd)];
}

std::vector<uint8_t> BleSender::encodePng(const uint8_t* rgb, int width, int height) {
    std::vector<uint8_t> png;
    unsigned error = lodepng::encode(png, rgb, (unsigned)width, (unsigned)height, LCT_RGB, 8);
    if (error) {
        std::cerr << "BleSender(macOS): PNG encode error: "
                  << lodepng_error_text(error) << std::endl;
        return {};
    }
    return png;
}

// ---- Stubs for the BlueZ-specific helpers (declared in the header but
//      not used on macOS). Kept as no-ops so the symbol table matches.
bool BleSender::dbusConnect() { return false; }
bool BleSender::bleConnect()  { return false; }
bool BleSender::bleWriteChar(const std::string&, const uint8_t*, size_t) { return false; }
bool BleSender::bleStartNotify(const std::string&)  { return false; }
std::string BleSender::findCharacteristic(const std::string&) { return ""; }
bool BleSender::waitForServicesResolved(int) { return false; }
std::string BleSender::devicePath() const { return m_addrDbus; }
