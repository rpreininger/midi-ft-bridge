// ====================================================================
//  Video Player - FFmpeg MP4 decoder with frame output
// ====================================================================
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <functional>

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

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Open an MP4 file and prepare for decoding
    // outWidth/outHeight: target resolution for output frames
    bool open(const std::string& path, int outWidth, int outHeight);

    // Decode and return the next frame as RGB24 pixels
    // Returns nullptr when finished or on error
    const uint8_t* nextFrame();

    // Rewind to the beginning of the clip
    void rewind();

    // Get the clip's FPS
    double getFPS() const;

    // Check if playback has reached the end
    bool isFinished() const { return m_finished; }

    // Audio packet callback: called for each audio packet during decode.
    // The packet is valid only during the callback (unref'd after return).
    using AudioPacketCallback = std::function<void(AVPacket* pkt)>;
    void setAudioCallback(AudioPacketCallback cb) { m_audioCallback = std::move(cb); }

    // Get audio stream codec parameters (nullptr if no audio stream)
    AVCodecParameters* getAudioCodecPar() const;
    AVRational getAudioTimeBase() const;

    // Close and release all resources
    void close();

private:
    bool decodeNextFrame();

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
    bool m_finished;

    AudioPacketCallback m_audioCallback;
    std::vector<uint8_t> m_rgbBuffer;
};
