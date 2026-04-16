// ====================================================================
//  Audio Player - ALSA PCM output with FFmpeg audio decoding
// ====================================================================
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>

// Forward declarations
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwrContext;
struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;
struct AVRational;

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // Open the ALSA PCM device. Call once at startup.
    bool init(const std::string& alsaDevice);

    // Prepare for a new clip's audio stream.
    // codecpar: audio stream codec parameters from the demuxer
    // timeBase: audio stream time base (num/den)
    bool openStream(AVCodecParameters* codecpar, int timeBaseNum, int timeBaseDen);

    // Feed a demuxed audio packet. Non-blocking (decodes + buffers PCM).
    void feedPacket(AVPacket* packet);

    // Flush the decoder (call at end of stream).
    void flush();

    // Stop current clip playback immediately.
    void stopClip();

    // Shut down ALSA device and worker thread.
    void close();

    bool isInitialized() const { return m_pcm != nullptr; }

private:
    void writerThread();
    void decodeAndBuffer(AVFrame* frame);
    void closeStream();

    // ALSA
    snd_pcm_t* m_pcm;
    int m_sampleRate;
    int m_channels;

    // FFmpeg audio decoder
    AVCodecContext* m_codecCtx;
    AVFrame* m_decFrame;

    // Resampler (source format -> S16LE stereo)
    SwrContext* m_swrCtx;

    // PCM ring buffer
    std::mutex m_bufMutex;
    std::condition_variable m_bufCv;
    std::vector<int16_t> m_ringBuf;
    size_t m_ringCapacity;  // in samples (not bytes)
    size_t m_readPos;
    size_t m_writePos;
    size_t m_available;     // samples available to read

    // Writer thread
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_playing;
    std::atomic<bool> m_prefilled;  // true once ring buffer has enough data to start
    std::chrono::steady_clock::time_point m_streamStart;
    size_t m_totalWritten;  // total frames written to ALSA
};
