// ====================================================================
//  Clip Player - Audio-master A/V orchestrator
// ====================================================================

#include "clip_player.h"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
}

ClipPlayer::ClipPlayer()
    : m_audio(nullptr)
    , m_hasAudio(false)
    , m_active(false)
    , m_useWallClock(false)
{
}

ClipPlayer::~ClipPlayer() {
    stop();
}

bool ClipPlayer::open(const std::string& clipPath, int videoWidth, int videoHeight,
                      AudioPlayer* audioPlayer) {
    stop();

    m_audio = audioPlayer;
    m_hasAudio = false;
    m_useWallClock = true;

    if (!m_video.open(clipPath, videoWidth, videoHeight)) {
        return false;
    }

    // Set up audio if the clip has an audio stream and we have an audio device
    if (m_audio && m_audio->isInitialized() && m_video.getAudioCodecPar()) {
        AVRational tb = m_video.getAudioTimeBase();
        if (m_audio->openStream(m_video.getAudioCodecPar(), tb.num, tb.den)) {
            // Route audio packets from the demux thread to the audio player
            m_video.setAudioCallback([this](AVPacket* pkt) {
                m_audio->feedPacket(pkt);
            });
            m_hasAudio = true;
            m_useWallClock = false;
            std::cerr << "ClipPlayer: Audio-master mode (synced to ALSA)" << std::endl;
        }
    }

    if (m_useWallClock) {
        std::cerr << "ClipPlayer: Wall-clock mode (no audio)" << std::endl;
    }

    m_wallClockStart = std::chrono::steady_clock::now();
    m_active = true;
    return true;
}

const uint8_t* ClipPlayer::getCurrentFrame() {
    if (!m_active) return nullptr;

    double position = getPosition();

    const uint8_t* frame = m_video.getFrameAtTime(position);
    if (!frame && m_video.isFinished()) {
        // Clip is done — flush audio
        if (m_hasAudio && m_audio) {
            m_audio->flush();
        }
        m_active = false;
        return nullptr;
    }

    return frame;
}

void ClipPlayer::stop() {
    if (!m_active) return;

    if (m_hasAudio && m_audio) {
        m_audio->stopClip();
    }
    m_video.close();
    m_active = false;
    m_hasAudio = false;
}

bool ClipPlayer::isFinished() const {
    return !m_active;
}

double ClipPlayer::getFPS() const {
    return m_video.getFPS();
}

double ClipPlayer::getPosition() const {
    if (!m_active) return 0.0;

    if (!m_useWallClock && m_audio && m_audio->isPlaying()) {
        return m_audio->getPlaybackPositionSec();
    }

    // Wall-clock fallback
    auto elapsed = std::chrono::steady_clock::now() - m_wallClockStart;
    return std::chrono::duration<double>(elapsed).count();
}
