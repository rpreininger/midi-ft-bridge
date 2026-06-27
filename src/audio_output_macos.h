// ====================================================================
//  Audio Output Device Selection - CoreAudio (macOS native)
//
//  The native macOS clip player decodes + plays audio through an
//  AudioQueue (see clip_player_macos.mm). By default an AudioQueue
//  renders to the system default output device. This module lets the
//  user pick *which* CoreAudio output interface that audio goes to,
//  without changing the audio decode/playback path itself:
//
//    * enumerate the currently-available output devices,
//    * hold a process-wide "selected device UID" (thread-safe),
//    * resolve a device by (substring of its) name for the config
//      default ("audio_output" in config.json).
//
//  clip_player_macos.mm reads getSelectedUID() when it creates each
//  AudioQueue and routes it via kAudioQueueProperty_CurrentDevice.
//
//  Pure C++ surface (no Objective-C in the header) so it can be included
//  from status_server.cpp / engine.cpp on the macOS build.
// ====================================================================
#pragma once

#include <string>

namespace macaudio {

// JSON snapshot of the current output devices, e.g.
//   {"selected":"<uid>","selectedName":"Roland FANTOM",
//    "devices":[{"uid":"...","name":"...","default":true}, ...]}
// "selected" is "" when routing to the system default device.
std::string devicesJSON();

// Select the output device by its CoreAudio UID. "" routes to the system
// default. Returns the human-readable name now in effect ("System Default"
// when uid is empty / not found).
std::string selectByUID(const std::string& uid);

// Select the first output device whose name contains nameSubstr
// (case-insensitive). Used to apply the config "audio_output" default.
// Returns the matched device name, or "" if no device matched (selection
// is left unchanged in that case).
std::string selectByNameSubstring(const std::string& nameSubstr);

// Currently selected device UID ("" = system default). Read by the clip
// player when it builds each AudioQueue.
std::string getSelectedUID();

// Human-readable name of the selected device ("System Default" if none).
std::string getSelectedName();

}  // namespace macaudio
