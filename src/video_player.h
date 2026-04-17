// ====================================================================
//  Video Player - Threaded FFmpeg MP4 decoder with PTS-indexed frames
// ====================================================================
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

extern "C" {
#include <libavutil/rational.h>
}

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// A decoded video frame with its presentation timestamp
struct DecodedFrame {
    std::vector<uint8_t> rgb;   // RGB24 pixel data
    double pts = -1.0;          // presentation timestamp in seconds
};

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Open an MP4 file and start the decode-ahead thread.
    // outWidth/outHeight: target resolution for output frames.
    bool open(const std::string& path, int outWidth, int outHeight);

    // Get the decoded frame closest to the given time (in seconds).
    // Returns pointer to RGB24 data, or nullptr if no frames available.
    // The pointer is valid until the next call to getFrameAtTime() or close().
    const uint8_t* getFrameAtTime(double timeSec);

    // Get the PTS of the last frame returned by getFrameAtTime().
    double getLastFramePTS() const { return m_lastPTS; }

    // Check if decode has finished (all frames consumed).
    bool isFinished() const { return m_finished && m_readIdx >= m_writeCount; }

    // Get the clip's FPS
    double getFPS() const;

    // Get clip duration in seconds
    double getDuration() const;

    // Audio packet callback: called from the demux thread for each audio packet.
    // The packet is valid only during the callback (unref'd after return).
    using AudioPacketCallback = std::function<void(AVPacket* pkt)>;
    void setAudioCallback(AudioPacketCallback cb) { m_audioCallback = std::move(cb); }

    // Get audio stream codec parameters (nullptr if no audio stream)
    AVCodecParameters* getAudioCodecPar() const;
    AVRational getAudioTimeBase() const;

    // Close and release all resources
    void close();

private:
    void decodeThread();
    bool decodeOneFrame();

    // FFmpeg demux/decode state
    AVFormatContext* m_formatCtx;
    AVCodecContext* m_codecCtx;
    AVFrame* m_frame;
    AVFrame* m_rgbFrame;
    AVPacket* m_packet;
    SwsContext* m_swsCtx;

    int m_videoStreamIdx;
    int m_audioStreamIdx;
    int m_outWidth;
    int m_outHeight;

    // Video stream time base for PTS conversion
    double m_videoTimeBase;

    // Decode-ahead frame ring buffer
    static constexpr int FRAME_BUF_SIZE = 8;
    DecodedFrame m_frameBuf[FRAME_BUF_SIZE];
    std::mutex m_bufMutex;
    std::condition_variable m_bufNotFull;   // signaled when consumer reads a frame
    std::condition_variable m_bufNotEmpty;  // signaled when producer decodes a frame
    int m_writeIdx;          // next slot to write (producer)
    int m_readIdx;           // next slot to read (consumer)
    std::atomic<int> m_writeCount;  // total frames written (monotonic)
    std::atomic<int> m_readCount;   // total frames read (monotonic)

    // The last frame returned to the consumer (stable pointer)
    DecodedFrame m_currentFrame;
    double m_lastPTS;

    // Decode thread
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_finished;  // decoder reached EOF

    AudioPacketCallback m_audioCallback;
    std::vector<uint8_t> m_rgbBuffer;  // temp buffer for sws_scale
};
