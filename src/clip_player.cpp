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
            std::cerr << "ClipPlayer: Audio-master mode" << std::endl;
        }
    }

    if (m_useWallClock) {
        std::cerr << "ClipPlayer: Wall-clock mode (no audio)" << std::endl;
    }

    // Only now start demux/decode. If we started them during m_video.open(),
    // the demux thread would race ahead and drop all audio packets before
    // the callback got wired up, starving the audio clock.
    m_video.start();

    m_wallClockStart = std::chrono::steady_clock::now();
    m_active = true;
    return true;
}

const uint8_t* ClipPlayer::getCurrentFrame() {
    if (!m_active) return nullptr;

    // Audio-master (or wall-clock fallback) drives the clock.
    // VideoPlayer returns the latest frame with PTS <= clock, or nullptr when
    // no new frame is due yet. nullptr means "keep showing the current frame";
    // we forward it so the main loop skips a redundant panel send.
    double clockSec = getPosition();
    const uint8_t* frame = m_video.getFrameAtTime(clockSec);

    if (!frame && m_video.isFinished()) {
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
    m_paused = false;
}

void ClipPlayer::pause() {
    if (!m_active || m_paused) return;
    if (m_useWallClock) {
        m_pauseStart = std::chrono::steady_clock::now();
    } else if (m_audio) {
        m_audio->pause();
    }
    m_paused = true;
}

void ClipPlayer::resume() {
    if (!m_active || !m_paused) return;
    if (m_useWallClock) {
        // Shift the virtual start forward by the paused duration so that
        // getPosition() resumes from the same timestamp.
        auto pausedFor = std::chrono::steady_clock::now() - m_pauseStart;
        m_wallClockStart += pausedFor;
    } else if (m_audio) {
        m_audio->resume();
    }
    m_paused = false;
}

bool ClipPlayer::isFinished() const {
    return !m_active;
}

double ClipPlayer::getFPS() const {
    return m_video.getFPS();
}

void ClipPlayer::seek(double targetSec) {
    if (!m_active) return;

    double dur = m_video.getDuration();
    if (targetSec < 0) targetSec = 0;
    if (dur > 0.0 && targetSec > dur - 0.1) targetSec = dur - 0.1;

    // Reset audio first so the first audio packet decoded after the demuxer
    // seek re-anchors the master clock to the new position.
    if (m_hasAudio && m_audio) m_audio->resetForSeek(targetSec);

    m_video.seek(targetSec);

    // Silent clips run off the wall clock — shift its origin to the target.
    if (m_useWallClock) {
        auto now = std::chrono::steady_clock::now();
        m_wallClockStart = now - std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::duration<double>(targetSec));
        if (m_paused) m_pauseStart = now;
    }
}

double ClipPlayer::getPosition() const {
    if (!m_active) return 0.0;

    if (!m_useWallClock && m_audio && m_audio->isPlaying()) {
        return m_audio->getPlaybackPositionSec();
    }

    // Wall-clock fallback — if paused, freeze at the pause moment.
    auto ref = m_paused ? m_pauseStart : std::chrono::steady_clock::now();
    auto elapsed = ref - m_wallClockStart;
    return std::chrono::duration<double>(elapsed).count();
}
