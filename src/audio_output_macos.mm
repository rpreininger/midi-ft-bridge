// ====================================================================
//  Audio Output Device Selection - CoreAudio implementation (macOS)
// ====================================================================

#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

#include "audio_output_macos.h"

#include <mutex>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {

std::mutex      g_mtx;
std::string     g_selectedUID;                 // "" = system default
std::string     g_selectedName = "System Default";

std::string cfToStd(CFStringRef s) {
    if (!s) return {};
    CFIndex len = CFStringGetLength(s);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(maxBytes));
    if (CFStringGetCString(s, buf.data(), maxBytes, kCFStringEncodingUTF8))
        return std::string(buf.data());
    return {};
}

// True if the device exposes at least one output channel.
bool deviceHasOutput(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr || size == 0)
        return false;
    std::vector<uint8_t> buf(size);
    auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, bl) != noErr)
        return false;
    UInt32 channels = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
        channels += bl->mBuffers[i].mNumberChannels;
    return channels > 0;
}

std::string deviceUID(AudioDeviceID dev) {
    CFStringRef uid = nullptr;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(uid);
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &uid) != noErr || !uid)
        return {};
    std::string s = cfToStd(uid);
    CFRelease(uid);
    return s;
}

std::string deviceName(AudioDeviceID dev) {
    CFStringRef name = nullptr;
    AudioObjectPropertyAddress addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name) != noErr || !name)
        return {};
    std::string s = cfToStd(name);
    CFRelease(name);
    return s;
}

AudioDeviceID defaultOutputDevice() {
    AudioDeviceID dev = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &dev);
    return dev;
}

struct DevInfo { std::string uid; std::string name; bool isDefault = false; };

std::vector<DevInfo> enumerateOutputs() {
    std::vector<DevInfo> out;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr
        || size == 0)
        return out;
    UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return out;

    AudioDeviceID def = defaultOutputDevice();
    for (AudioDeviceID id : ids) {
        if (!deviceHasOutput(id)) continue;
        DevInfo d;
        d.uid       = deviceUID(id);
        d.name      = deviceName(id);
        d.isDefault = (id == def);
        if (!d.uid.empty()) out.push_back(std::move(d));
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

namespace macaudio {

std::string devicesJSON() {
    std::vector<DevInfo> devs = enumerateOutputs();

    std::string selUID, selName;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        selUID  = g_selectedUID;
        selName = g_selectedName;
    }

    std::string json = "{\"selected\":\"" + jsonEscape(selUID) +
                       "\",\"selectedName\":\"" + jsonEscape(selName) +
                       "\",\"devices\":[";
    for (size_t i = 0; i < devs.size(); ++i) {
        if (i) json += ",";
        json += "{\"uid\":\"" + jsonEscape(devs[i].uid) +
                "\",\"name\":\"" + jsonEscape(devs[i].name) +
                "\",\"default\":" + (devs[i].isDefault ? "true" : "false") + "}";
    }
    json += "]}";
    return json;
}

std::string selectByUID(const std::string& uid) {
    std::string name = "System Default";
    if (!uid.empty()) {
        for (const auto& d : enumerateOutputs()) {
            if (d.uid == uid) { name = d.name; break; }
        }
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    // Keep UID even if it isn't currently present (device may reappear),
    // but only when the caller actually passed one.
    g_selectedUID  = uid;
    g_selectedName = name;
    return name;
}

std::string selectByNameSubstring(const std::string& nameSubstr) {
    if (nameSubstr.empty()) return {};
    std::string needle = toLower(nameSubstr);
    for (const auto& d : enumerateOutputs()) {
        if (toLower(d.name).find(needle) != std::string::npos) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_selectedUID  = d.uid;
            g_selectedName = d.name;
            return d.name;
        }
    }
    return {};
}

std::string getSelectedUID() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_selectedUID;
}

std::string getSelectedName() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_selectedName;
}

}  // namespace macaudio
