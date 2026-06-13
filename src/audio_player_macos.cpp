// ====================================================================
//  Audio Player - SDL2 audio implementation for macOS
//  Uses SDL_QueueAudio (push model) — dead simple, no callbacks.
// ====================================================================

#include "audio_player.h"
#include <iostream>
#include <cstring>
#include <algorithm>

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

static constexpr int OUT_CHANNELS = 2;

// SDL audio device ID stored as opaque pointer via m_pcm.
// totalBytesQueued is cumulative across the lifetime of the current stream;
// combined with SDL_GetQueuedAudioSize() it gives the real playback position.
struct SDLAudioState {
    SDL_AudioDeviceID device;
    std::atomic<size_t> totalBytesQueued{0};
};

AudioPlayer::AudioPlayer()
    : m_pcm(nullptr)
    , m_sampleRate(0)
    , m_channels(OUT_CHANNELS)
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

bool AudioPlayer::init(const std::string& device) {
    (void)device;

    // Keep Ctrl+C / SIGTERM ours — SDL will otherwise install handlers that
    // just set an internal flag we don't poll, and the process stops responding.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "AudioPlayer: SDL_Init(AUDIO) failed: " << SDL_GetError() << std::endl;
        return false;
    }

    auto* state = new SDLAudioState{};
    state->device = 0;
    m_pcm = reinterpret_cast<snd_pcm_t*>(state);
    m_running = true;

    // Start clock thread
    m_thread = std::thread(&AudioPlayer::writerThread, this);

    std::cerr << "AudioPlayer: SDL2 audio initialized" << std::endl;
    return true;
}

bool AudioPlayer::openStream(AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen) {
    m_audioTbNum = timeBaseNum;
    m_audioTbDen = (timeBaseDen > 0) ? timeBaseDen : 1;

    closeStream();

    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (!state) return false;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "AudioPlayer: Unsupported audio codec" << std::endl;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, codecpar);
    m_codecCtx->thread_count = 1;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "AudioPlayer: Failed to open audio codec" << std::endl;
        closeStream();
        return false;
    }

    m_decFrame = av_frame_alloc();

    m_sampleRate = m_codecCtx->sample_rate;
    if (m_sampleRate <= 0) m_sampleRate = 44100;

    // Set up resampler
    m_swrCtx = swr_alloc();
    if (!m_swrCtx) {
        closeStream();
        return false;
    }

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout;
    if (m_codecCtx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&inLayout, &m_codecCtx->ch_layout);
    } else {
        inLayout = AV_CHANNEL_LAYOUT_STEREO;
    }

    swr_alloc_set_opts2(&m_swrCtx,
                        &outLayout, AV_SAMPLE_FMT_FLT, m_sampleRate,
                        &inLayout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
                        0, nullptr);

    if (swr_init(m_swrCtx) < 0) {
        std::cerr << "AudioPlayer: Failed to init resampler" << std::endl;
        closeStream();
        return false;
    }

    // Open SDL audio device
    SDL_AudioSpec want = {}, have = {};
    want.freq = m_sampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = OUT_CHANNELS;
    want.samples = 2048;
    want.callback = nullptr;  // push mode (SDL_QueueAudio)

    state->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (state->device == 0) {
        std::cerr << "AudioPlayer: SDL_OpenAudioDevice failed: " << SDL_GetError() << std::endl;
        closeStream();
        return false;
    }

    m_sampleRate = have.freq;
    m_playing = true;
    m_prefilled = true;  // no prefill needed with SDL push model
    m_streamStart = std::chrono::steady_clock::now();
    m_totalWritten = 0;
    state->totalBytesQueued = 0;
    m_clockBaseSec = 0.0;       // fresh clip starts at content time 0
    m_haveClockBase = true;     // don't re-anchor during normal playback

    // Unpause SDL audio
    SDL_PauseAudioDevice(state->device, 0);

    std::cerr << "AudioPlayer: Stream opened ("
              << m_codecCtx->sample_rate << "Hz "
              << m_codecCtx->ch_layout.nb_channels << "ch -> "
              << m_sampleRate << "Hz " << OUT_CHANNELS << "ch FLOAT, SDL2)" << std::endl;

    return true;
}

void AudioPlayer::feedPacket(AVPacket* packet) {
    if (!m_codecCtx || !m_playing) return;

    // After a seek, anchor the clock to the first packet's real timestamp.
    if (!m_haveClockBase.load() && packet && packet->pts != AV_NOPTS_VALUE) {
        m_clockBaseSec = (double)packet->pts * m_audioTbNum / m_audioTbDen;
        m_haveClockBase = true;
    }

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) return;

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_codecCtx, m_decFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        decodeAndBuffer(m_decFrame);
    }
}

void AudioPlayer::flush() {
    if (!m_codecCtx || !m_playing) return;

    avcodec_send_packet(m_codecCtx, nullptr);
    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_frame(m_codecCtx, m_decFrame);
        if (ret < 0) break;
        decodeAndBuffer(m_decFrame);
    }
}

void AudioPlayer::resetForSeek(double baseHintSec) {
    if (!m_playing) return;

    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state && state->device) {
        SDL_ClearQueuedAudio(state->device);
        state->totalBytesQueued = 0;
    }
    if (m_codecCtx) avcodec_flush_buffers(m_codecCtx);

    m_totalWritten = 0;
    m_clockBaseSec = baseHintSec;   // report ~target until the first packet lands
    m_haveClockBase = false;        // next packet re-anchors to its exact PTS
}

void AudioPlayer::decodeAndBuffer(AVFrame* frame) {
    int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
    if (outSamples <= 0) return;

    size_t needed = outSamples * OUT_CHANNELS;
    if (m_resampleBuf.size() < needed) {
        m_resampleBuf.resize(needed);
    }
    uint8_t* outBuf = reinterpret_cast<uint8_t*>(m_resampleBuf.data());

    int converted = swr_convert(m_swrCtx,
                                &outBuf, outSamples,
                                (const uint8_t**)frame->extended_data, frame->nb_samples);
    if (converted <= 0) return;

    // Push directly to SDL audio queue — no ring buffer needed
    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state && state->device) {
        uint32_t bytes = converted * OUT_CHANNELS * sizeof(float);
        SDL_QueueAudio(state->device, m_resampleBuf.data(), bytes);
        state->totalBytesQueued += bytes;
    }
}

void AudioPlayer::pause() {
    m_paused = true;
    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state && state->device) {
        SDL_PauseAudioDevice(state->device, 1);
    }
}

void AudioPlayer::resume() {
    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state && state->device) {
        SDL_PauseAudioDevice(state->device, 0);
    }
    m_paused = false;
}

void AudioPlayer::stopClip() {
    m_playing = false;
    m_paused = false;

    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state && state->device) {
        SDL_ClearQueuedAudio(state->device);
        SDL_CloseAudioDevice(state->device);
        state->device = 0;
    }

    closeStream();
}

void AudioPlayer::closeStream() {
    if (m_swrCtx) { swr_free(&m_swrCtx); m_swrCtx = nullptr; }
    if (m_decFrame) { av_frame_free(&m_decFrame); m_decFrame = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
}

void AudioPlayer::close() {
    m_playing = false;
    m_running = false;

    if (m_thread.joinable()) {
        m_thread.join();
    }

    closeStream();

    auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
    if (state) {
        if (state->device) {
            SDL_CloseAudioDevice(state->device);
        }
        delete state;
        m_pcm = nullptr;
    }
}

size_t AudioPlayer::pullFrames(float* dst, size_t maxFrames) {
    (void)dst; (void)maxFrames;
    return 0;  // not used — SDL push model
}

void AudioPlayer::writerThread() {
    // Master clock: frames actually consumed by the SDL audio device.
    // totalBytesQueued is cumulative; SDL_GetQueuedAudioSize() is what's still
    // pending. The difference is what has been handed to the audio hardware.
    const size_t bytesPerFrame = OUT_CHANNELS * sizeof(float);
    while (m_running) {
        if (m_playing) {
            auto* state = reinterpret_cast<SDLAudioState*>(m_pcm);
            if (state && state->device) {
                size_t queuedNow = SDL_GetQueuedAudioSize(state->device);
                size_t totalQueued = state->totalBytesQueued.load();
                size_t consumed = (totalQueued > queuedNow) ? (totalQueued - queuedNow) : 0;
                m_totalWritten = consumed / bytesPerFrame;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
