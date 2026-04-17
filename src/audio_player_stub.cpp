// ====================================================================
//  Audio Player - Stub implementation for macOS
//  Reports as uninitialized so ClipPlayer uses wall-clock fallback.
// ====================================================================

#include "audio_player.h"
#include <iostream>

// Forward-declare FFmpeg types used in the header
extern "C" {
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
}

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
    , m_totalWritten(0)
{
}

AudioPlayer::~AudioPlayer() {
    close();
}

bool AudioPlayer::init(const std::string& alsaDevice) {
    (void)alsaDevice;
    std::cerr << "AudioPlayer: Stub mode (no ALSA). Audio disabled." << std::endl;
    // Return false so isInitialized() stays false and ClipPlayer uses wall clock
    return false;
}

bool AudioPlayer::openStream(AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen) {
    (void)codecpar; (void)timeBaseNum; (void)timeBaseDen;
    return false;
}

void AudioPlayer::feedPacket(AVPacket* packet) { (void)packet; }
void AudioPlayer::flush() {}
void AudioPlayer::stopClip() {}
void AudioPlayer::close() {}
void AudioPlayer::writerThread() {}
void AudioPlayer::decodeAndBuffer(AVFrame* frame) { (void)frame; }
void AudioPlayer::closeStream() {}
size_t AudioPlayer::pullFrames(float* dst, size_t maxFrames) {
    (void)dst; (void)maxFrames; return 0;
}
