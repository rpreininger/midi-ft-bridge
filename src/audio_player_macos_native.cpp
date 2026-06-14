// ====================================================================
//  Audio Player - Native macOS no-op implementation
//
//  On the native macOS build (MFB_NATIVE_MACOS), audio is decoded and
//  played by AVPlayer inside clip_player_macos.mm, and A/V sync is handled
//  by AVFoundation. The engine still owns an AudioPlayer instance and calls
//  init()/close() on it, so this provides a minimal, dependency-free shell
//  (no SDL2, no FFmpeg, no ALSA). The packet-feeding methods are never
//  reached on this build but are defined so the interface links.
// ====================================================================

#include "audio_player.h"
#include <iostream>

AudioPlayer::AudioPlayer()
    : m_pcm(nullptr)
    , m_sampleRate(0)
    , m_channels(2)
    , m_codecCtx(nullptr)
    , m_decFrame(nullptr)
    , m_swrCtx(nullptr)
    , m_ringCapacity(0)
    , m_readPos(0)
    , m_writePos(0)
    , m_available(0)
    , m_running(false)
    , m_playing(false)
    , m_prefilled(false)
{
}

AudioPlayer::~AudioPlayer() {
    close();
}

bool AudioPlayer::init(const std::string& device) {
    (void)device;
    // Audio output is owned by AVPlayer (see clip_player_macos.mm).
    std::cerr << "AudioPlayer: native macOS mode (audio via AVFoundation)" << std::endl;
    return true;
}

bool AudioPlayer::openStream(AVCodecParameters*, int, int) { return false; }
void AudioPlayer::feedPacket(AVPacket*) {}
void AudioPlayer::flush() {}
void AudioPlayer::resetForSeek(double) {}
void AudioPlayer::stopClip() { m_playing = false; }
void AudioPlayer::pause() { m_paused = true; }
void AudioPlayer::resume() { m_paused = false; }
void AudioPlayer::close() { m_running = false; m_playing = false; }
size_t AudioPlayer::pullFrames(float*, size_t) { return 0; }
