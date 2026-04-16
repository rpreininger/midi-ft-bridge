// ====================================================================
//  Audio Player - ALSA PCM output with FFmpeg audio decoding
// ====================================================================

#include "audio_player.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <alsa/asoundlib.h>
}

// Ring buffer capacity in seconds (at 48kHz stereo)
static constexpr size_t RING_BUF_SECONDS = 10;
static constexpr int OUT_CHANNELS = 2;
static constexpr int OUT_SAMPLE_FMT = AV_SAMPLE_FMT_FLT;

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
{
}

AudioPlayer::~AudioPlayer() {
    close();
}

bool AudioPlayer::init(const std::string& alsaDevice) {
    int err = snd_pcm_open(&m_pcm, alsaDevice.c_str(),
                           SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "AudioPlayer: Failed to open ALSA device '"
                  << alsaDevice << "': " << snd_strerror(err) << std::endl;
        m_pcm = nullptr;
        return false;
    }

    // Configure initial params so ALSA doesn't output garbage
    snd_pcm_set_params(m_pcm,
                       SND_PCM_FORMAT_FLOAT_LE,
                       SND_PCM_ACCESS_RW_INTERLEAVED,
                       OUT_CHANNELS, 48000,
                       0, 500000);
    snd_pcm_drop(m_pcm);
    snd_pcm_prepare(m_pcm);

    m_running = true;
    m_thread = std::thread(&AudioPlayer::writerThread, this);

    std::cerr << "AudioPlayer: Opened ALSA device '" << alsaDevice << "'" << std::endl;
    return true;
}

bool AudioPlayer::openStream(AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen) {
    (void)timeBaseNum;
    (void)timeBaseDen;

    closeStream();

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

    // Determine source sample rate
    m_sampleRate = m_codecCtx->sample_rate;
    if (m_sampleRate <= 0) m_sampleRate = 44100;

    // Set up resampler: source format -> S16LE stereo at source sample rate
    m_swrCtx = swr_alloc();
    if (!m_swrCtx) {
        std::cerr << "AudioPlayer: Failed to allocate resampler" << std::endl;
        closeStream();
        return false;
    }

    // Set resampler options
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

    // Configure ALSA for this stream's sample rate (FLOAT_LE = Fantom native format)
    int err = snd_pcm_set_params(m_pcm,
                                 SND_PCM_FORMAT_FLOAT_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 OUT_CHANNELS,
                                 m_sampleRate,
                                 0,       // no software resampling (native format)
                                 2000000); // 2s latency
    if (err < 0) {
        std::cerr << "AudioPlayer: Failed to set ALSA params: "
                  << snd_strerror(err) << std::endl;
        closeStream();
        return false;
    }

    // Reset ALSA and prime with silence to prevent initial underrun
    snd_pcm_drop(m_pcm);
    snd_pcm_prepare(m_pcm);
    {
        snd_pcm_uframes_t bufferSize;
        snd_pcm_uframes_t periodSize;
        snd_pcm_get_params(m_pcm, &bufferSize, &periodSize);
        std::vector<float> silence(bufferSize * OUT_CHANNELS, 0.0f);
        snd_pcm_writei(m_pcm, silence.data(), bufferSize);
    }

    // Allocate ring buffer
    m_ringCapacity = m_sampleRate * OUT_CHANNELS * RING_BUF_SECONDS;
    m_ringBuf.resize(m_ringCapacity);
    m_readPos = 0;
    m_writePos = 0;
    m_available = 0;

    m_prefilled = true;  // ALSA already primed with silence
    m_playing = true;
    m_streamStart = std::chrono::steady_clock::now();
    m_totalWritten = 0;

    std::cerr << "AudioPlayer: Stream opened ("
              << m_codecCtx->sample_rate << "Hz "
              << m_codecCtx->ch_layout.nb_channels << "ch -> "
              << m_sampleRate << "Hz " << OUT_CHANNELS << "ch FLOAT)" << std::endl;

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
    // Resample to FLOAT_LE stereo (Fantom native format)
    int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
    if (outSamples <= 0) return;

    // Resize pre-allocated buffer if needed (no alloc after first few calls)
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

    // Write to ring buffer
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);

        for (size_t i = 0; i < samplesTotal; i++) {
            if (m_available >= m_ringCapacity) {
                // Buffer full — drop oldest samples
                m_readPos = (m_readPos + 1) % m_ringCapacity;
                m_available--;
            }
            m_ringBuf[m_writePos] = m_resampleBuf[i];
            m_writePos = (m_writePos + 1) % m_ringCapacity;
            m_available++;
        }
    }

    // Signal prefilled once we have 1s of audio buffered
    if (!m_prefilled && m_available >= (size_t)(m_sampleRate * OUT_CHANNELS)) {
        m_prefilled = true;
    }
    m_bufCv.notify_one();
}

void AudioPlayer::stopClip() {
    m_playing = false;

    if (m_pcm) {
        snd_pcm_drop(m_pcm);
    }

    // Clear ring buffer
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        m_readPos = 0;
        m_writePos = 0;
        m_available = 0;
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
    m_bufCv.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    closeStream();

    if (m_pcm) {
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
}

void AudioPlayer::writerThread() {
    // Elevate thread priority to avoid starvation during heavy video decode
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        // Fallback: at least try a nice value
        nice(-10);
    }

    const size_t framesPerWrite = 1024;
    std::vector<float> writeBuf(framesPerWrite * OUT_CHANNELS);

    while (m_running) {
        size_t framesToWrite = 0;
        {
            std::unique_lock<std::mutex> lock(m_bufMutex);
            // Timed wait: wake on new data or every 5ms to keep ALSA fed
            m_bufCv.wait_for(lock, std::chrono::milliseconds(5), [&] {
                return !m_running || (m_playing && m_prefilled && m_available > 0);
            });

            if (!m_running) break;
            if (!m_playing) continue;

            // Write whatever is available, up to framesPerWrite
            size_t availFrames = m_available / OUT_CHANNELS;
            framesToWrite = std::min(availFrames, framesPerWrite);
            size_t samplesToRead = framesToWrite * OUT_CHANNELS;

            for (size_t i = 0; i < samplesToRead; i++) {
                writeBuf[i] = m_ringBuf[m_readPos];
                m_readPos = (m_readPos + 1) % m_ringCapacity;
            }
            m_available -= samplesToRead;
        }

        if (!m_pcm || !m_playing || framesToWrite == 0) continue;

        // Write to ALSA
        snd_pcm_sframes_t written = snd_pcm_writei(m_pcm, writeBuf.data(), framesToWrite);
        if (written > 0) {
            m_totalWritten += written;
        }
        if (written == -EPIPE) {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_streamStart).count();
            double audioPos = (double)m_totalWritten / m_sampleRate;
            std::cerr << "AudioPlayer: underrun at wall=" << elapsed
                      << "s audio=" << audioPos << "s" << std::endl;
            snd_pcm_prepare(m_pcm);
        } else if (written == -ESTRPIPE) {
            while (snd_pcm_resume(m_pcm) == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            snd_pcm_prepare(m_pcm);
        }
    }
}
