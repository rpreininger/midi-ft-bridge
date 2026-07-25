// ====================================================================
//  Audio Output Device Selection - Apple implementations
//
//  macOS uses the CoreAudio HAL; iOS uses AVAudioSession. They differ in
//  kind, not just in API: on macOS the app picks the output device, on
//  iOS the system does. See the iOS section for the consequences.
// ====================================================================

#include <TargetConditionals.h>

#include "audio_output_macos.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <vector>

namespace {

struct DevInfo { std::string uid; std::string name; bool isDefault = false; };

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

// Shared by both platforms: the JSON shape the web UI and the app's picker
// consume. Which devices land in `devs` is what differs.
std::string buildDevicesJSON(const std::vector<DevInfo>& devs,
                             const std::string& selUID,
                             const std::string& selName) {
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

}  // namespace

// ====================================================================
#if TARGET_OS_OSX
// ====================================================================
//  macOS - CoreAudio HAL. Real enumeration, real selection.

#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

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

}  // namespace

namespace macaudio {

std::vector<std::string> outputDeviceNames() {
    std::vector<std::string> names;
    for (const auto& d : enumerateOutputs()) names.push_back(d.name);
    return names;
}

std::string devicesJSON() {
    std::vector<DevInfo> devs = enumerateOutputs();
    std::string selUID, selName;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        selUID  = g_selectedUID;
        selName = g_selectedName;
    }
    return buildDevicesJSON(devs, selUID, selName);
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

// An AudioQueue on macOS renders without any session setup.
void prepareForPlayback() {}

}  // namespace macaudio

// ====================================================================
#else   // TARGET_OS_IPHONE (and simulator)
// ====================================================================
//  iOS - AVAudioSession.
//
//  There is no device picker on iOS. The system resolves the output route
//  by priority (USB / BT / headphones / speaker) and the app only gets to
//  observe it. So this implementation reports the *current route* through
//  the same surface, and treats selection as advisory:
//
//    * getSelectedUID() always returns "" - the clip player must not try
//      to pin an AudioQueue to a device (that property is macOS-only).
//    * getSelectedName() is the live route, not a stored preference.
//    * selectByNameSubstring() only confirms a match; it cannot force one.
//
//  Practical effect for this app: attach the Fantom (or any USB audio
//  interface) and it *becomes* the route on its own. What is lost is
//  choosing between several attached outputs, and the config's
//  "audio_output" key is therefore a check, not a command.

#import <AVFoundation/AVFoundation.h>

namespace {

std::string nsToStd(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string();
}

// The outputs of the route the system is currently using. Empty until the
// session has been activated at least once (see prepareForPlayback).
std::vector<DevInfo> currentRouteOutputs() {
    std::vector<DevInfo> out;
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
            DevInfo d;
            d.uid  = nsToStd(port.UID);
            d.name = nsToStd(port.portName);
            d.isDefault = true;           // the system picked it, so it is in use
            if (!d.uid.empty()) out.push_back(std::move(d));
        }
    }
    return out;
}

std::string currentRouteName() {
    std::vector<DevInfo> devs = currentRouteOutputs();
    if (devs.empty()) return "System Default";
    // Multi-output routes are rare; name them all rather than pick one.
    std::string name = devs[0].name;
    for (size_t i = 1; i < devs.size(); ++i) name += " + " + devs[i].name;
    return name;
}

}  // namespace

namespace macaudio {

std::vector<std::string> outputDeviceNames() {
    std::vector<std::string> names;
    for (const auto& d : currentRouteOutputs()) names.push_back(d.name);
    return names;
}

std::string devicesJSON() {
    // "selected" stays empty: on iOS nothing is selectable, so the UI should
    // render this as a status line rather than a picker.
    return buildDevicesJSON(currentRouteOutputs(), "", currentRouteName());
}

std::string selectByUID(const std::string& /*uid*/) {
    // Nothing to select. Report what is actually playing so callers that
    // echo the return value stay truthful.
    return currentRouteName();
}

std::string selectByNameSubstring(const std::string& nameSubstr) {
    if (nameSubstr.empty()) return {};
    std::string needle = toLower(nameSubstr);
    for (const auto& d : currentRouteOutputs()) {
        if (toLower(d.name).find(needle) != std::string::npos) return d.name;
    }
    return {};   // caller logs "not found"; on iOS that means "cannot force"
}

std::string getSelectedUID() {
    return {};   // never pin an AudioQueue to a device on iOS
}

std::string getSelectedName() {
    return currentRouteName();
}

void prepareForPlayback() {
    static std::once_flag once;
    std::call_once(once, []{
        @autoreleasepool {
            AVAudioSession* session = [AVAudioSession sharedInstance];
            NSError* err = nil;
            // Playback category: keeps audio alive when the screen locks and
            // ignores the ring/silent switch - both required for a live set.
            if (![session setCategory:AVAudioSessionCategoryPlayback
                                 mode:AVAudioSessionModeDefault
                              options:0
                                error:&err]) {
                std::cerr << "AudioOutput(iOS): setCategory failed: "
                          << nsToStd(err.localizedDescription) << std::endl;
            }
            err = nil;
            if (![session setActive:YES error:&err]) {
                std::cerr << "AudioOutput(iOS): setActive failed: "
                          << nsToStd(err.localizedDescription) << std::endl;
            }
        }
    });
}

}  // namespace macaudio

#endif  // TARGET_OS_OSX
