// ====================================================================
//  Audio Player - CoreAudio implementation for macOS
//  Uses AudioQueue for PCM output with FFmpeg audio decoding.
//  The AudioQueue callback pulls from the PCM ring buffer directly.
// ====================================================================

#include "audio_player.h"
#include <iostream>
#include <cstring>
#include <algorithm>

#include <AudioToolbox/AudioToolbox.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

static constexpr size_t RING_BUF_SECONDS = 10;
static constexpr int OUT_CHANNELS = 2;
static constexpr int NUM_AQ_BUFFERS = 3;
static constexpr int AQ_BUFFER_FRAMES = 2048;

struct CoreAudioState {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_AQ_BUFFERS];
    AudioPlayer* player;
};

// AudioQueue callback: pull PCM data from the player's ring buffer
static void aqOutputCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* player = static_cast<AudioPlayer*>(userData);

    size_t framesRequested = buffer->mAudioDataBytesCapacity / (OUT_CHANNELS * sizeof(float));
    auto* dst = static_cast<float*>(buffer->mAudioData);

    size_t framesWritten = player->pullFrames(dst, framesRequested);

    // Pad with silence if we didn't have enough data
    if (framesWritten < framesRequested) {
        memset(dst + framesWritten * OUT_CHANNELS, 0,
               (framesRequested - framesWritten) * OUT_CHANNELS * sizeof(float));
    }

    buffer->mAudioDataByteSize = framesRequested * OUT_CHANNELS * sizeof(float);
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

size_t AudioPlayer::pullFrames(float* dst, size_t maxFrames) {
    std::lock_guard<std::mutex> lock(m_bufMutex);

    if (!m_playing || !m_prefilled) {
        return 0;
    }

    size_t availFrames = m_available / OUT_CHANNELS;
    size_t framesWritten = std::min(availFrames, maxFrames);
    size_t samplesToRead = framesWritten * OUT_CHANNELS;

    for (size_t i = 0; i < samplesToRead; i++) {
        dst[i] = m_ringBuf[m_readPos];
        m_readPos = (m_readPos + 1) % m_ringCapacity;
    }
    m_available -= samplesToRead;
    m_totalWritten += framesWritten;

    return framesWritten;
}

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

    auto* state = new CoreAudioState{};
    state->queue = nullptr;
    state->player = this;

    m_pcm = reinterpret_cast<snd_pcm_t*>(state);
    m_running = true;

    std::cerr << "AudioPlayer: CoreAudio initialized" << std::endl;
    return true;
}

bool AudioPlayer::openStream(AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen) {
    (void)timeBaseNum;
    (void)timeBaseDen;

    closeStream();

    auto* state = reinterpret_cast<CoreAudioState*>(m_pcm);
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

    // Create AudioQueue
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = m_sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 32;
    fmt.mChannelsPerFrame = OUT_CHANNELS;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = OUT_CHANNELS * sizeof(float);
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    OSStatus err = AudioQueueNewOutput(&fmt, aqOutputCallback, this,
                                       nullptr, nullptr, 0, &state->queue);
    if (err != noErr) {
        std::cerr << "AudioPlayer: AudioQueueNewOutput failed: " << err << std::endl;
        closeStream();
        return false;
    }

    // Allocate and prime AudioQueue buffers
    UInt32 bufferSize = AQ_BUFFER_FRAMES * fmt.mBytesPerFrame;
    for (int i = 0; i < NUM_AQ_BUFFERS; i++) {
        AudioQueueAllocateBuffer(state->queue, bufferSize, &state->buffers[i]);
        memset(state->buffers[i]->mAudioData, 0, bufferSize);
        state->buffers[i]->mAudioDataByteSize = bufferSize;
        AudioQueueEnqueueBuffer(state->queue, state->buffers[i], 0, nullptr);
    }

    // Allocate ring buffer
    m_ringCapacity = m_sampleRate * OUT_CHANNELS * RING_BUF_SECONDS;
    m_ringBuf.resize(m_ringCapacity);
    m_readPos = 0;
    m_writePos = 0;
    m_available = 0;
    m_prefilled = false;
    m_totalWritten = 0;
    m_playing = true;
    m_streamStart = std::chrono::steady_clock::now();

    // Start AudioQueue playback
    AudioQueueStart(state->queue, nullptr);

    std::cerr << "AudioPlayer: Stream opened ("
              << m_codecCtx->sample_rate << "Hz "
              << m_codecCtx->ch_layout.nb_channels << "ch -> "
              << m_sampleRate << "Hz " << OUT_CHANNELS << "ch FLOAT, CoreAudio)" << std::endl;

    return true;
}

void AudioPlayer::feedPacket(AVPacket* packet) {
    if (!m_codecCtx || !m_playing) return;

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

    size_t samplesTotal = converted * OUT_CHANNELS;

    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        for (size_t i = 0; i < samplesTotal; i++) {
            if (m_available >= m_ringCapacity) {
                m_readPos = (m_readPos + 1) % m_ringCapacity;
                m_available--;
            }
            m_ringBuf[m_writePos] = m_resampleBuf[i];
            m_writePos = (m_writePos + 1) % m_ringCapacity;
            m_available++;
        }
    }

    if (!m_prefilled && m_available >= (size_t)(m_sampleRate * OUT_CHANNELS)) {
        m_prefilled = true;
    }
}

void AudioPlayer::stopClip() {
    m_playing = false;

    auto* state = reinterpret_cast<CoreAudioState*>(m_pcm);
    if (state && state->queue) {
        AudioQueueStop(state->queue, true);
    }

    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        m_readPos = 0;
        m_writePos = 0;
        m_available = 0;
    }

    closeStream();
}

void AudioPlayer::closeStream() {
    auto* state = reinterpret_cast<CoreAudioState*>(m_pcm);
    if (state && state->queue) {
        AudioQueueStop(state->queue, true);
        AudioQueueDispose(state->queue, true);
        state->queue = nullptr;
    }

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

    auto* state = reinterpret_cast<CoreAudioState*>(m_pcm);
    if (state) {
        delete state;
        m_pcm = nullptr;
    }
}

void AudioPlayer::writerThread() {
    // CoreAudio uses a callback model — the AudioQueue callback
    // (aqOutputCallback) pulls from the ring buffer directly.
    // This thread is only needed to wait for prefill before starting.
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
