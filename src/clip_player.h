// ====================================================================
//  Clip Player - Audio-master A/V orchestrator
//
//  Owns a VideoPlayer and coordinates with a shared AudioPlayer.
//  The audio playback position is the master clock; video frames are
//  picked to match. Falls back to wall-clock when there's no audio.
// ====================================================================
#pragma once

#include "video_player.h"
#include "audio_player.h"
#include <string>
#include <chrono>
#include <atomic>

struct Config;

class ClipPlayer {
public:
    ClipPlayer();
    ~ClipPlayer();

    // Open a clip file and start decoding.
    // audioPlayer: shared AudioPlayer instance (may be null/uninitialized).
    bool open(const std::string& clipPath, int videoWidth, int videoHeight,
              AudioPlayer* audioPlayer);

    // Get the current frame to display, synced to the audio clock.
    // Returns pointer to RGB24 data, or nullptr if clip is finished.
    const uint8_t* getCurrentFrame();

    // Stop playback immediately.
    void stop();

    // Is the clip finished playing?
    bool isFinished() const;

    // Does this clip have audio?
    bool hasAudio() const { return m_hasAudio; }

    // Get the video FPS (for panel send rate hints)
    double getFPS() const;

    // Get current playback position in seconds
    double getPosition() const;

private:
    VideoPlayer m_video;
    AudioPlayer* m_audio;  // not owned, shared across clips

    bool m_hasAudio;
    bool m_active;

    // Wall-clock fallback for clips without audio
    std::chrono::steady_clock::time_point m_wallClockStart;
    bool m_useWallClock;
};
