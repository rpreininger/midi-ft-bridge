// ====================================================================
//  Video Player - FFmpeg MP4 decoder implementation
// ====================================================================

#include "video_player.h"
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

VideoPlayer::VideoPlayer()
    : m_formatCtx(nullptr)
    , m_codecCtx(nullptr)
    , m_frame(nullptr)
    , m_rgbFrame(nullptr)
    , m_packet(nullptr)
    , m_swsCtx(nullptr)
    , m_videoStreamIdx(-1)
    , m_audioStreamIdx(-1)
    , m_outWidth(0)
    , m_outHeight(0)
    , m_finished(true)
{
}

VideoPlayer::~VideoPlayer() {
    close();
}

bool VideoPlayer::open(const std::string& path, int outWidth, int outHeight) {
    close();

    m_outWidth = outWidth;
    m_outHeight = outHeight;
    m_finished = false;

    // Open input file
    if (avformat_open_input(&m_formatCtx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "VideoPlayer: Failed to open " << path << std::endl;
        close();
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        std::cerr << "VideoPlayer: Failed to find stream info" << std::endl;
        close();
        return false;
    }

    // Find the video and audio streams
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_videoStreamIdx < 0) {
            m_videoStreamIdx = i;
        } else if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audioStreamIdx < 0) {
            m_audioStreamIdx = i;
        }
    }

    if (m_videoStreamIdx < 0) {
        std::cerr << "VideoPlayer: No video stream found in " << path << std::endl;
        close();
        return false;
    }

    // Find and open decoder
    AVCodecParameters* codecpar = m_formatCtx->streams[m_videoStreamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "VideoPlayer: Unsupported codec" << std::endl;
        close();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, codecpar);

    // Pi Zero optimization: single thread decode
    m_codecCtx->thread_count = 1;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "VideoPlayer: Failed to open codec" << std::endl;
        close();
        return false;
    }

    // Allocate frames
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    // Allocate RGB buffer
    int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, outWidth, outHeight, 1);
    m_rgbBuffer.resize(bufSize);
    av_image_fill_arrays(m_rgbFrame->data, m_rgbFrame->linesize,
                         m_rgbBuffer.data(), AV_PIX_FMT_RGB24,
                         outWidth, outHeight, 1);

    // Create scaler (YUV -> RGB24 at target resolution)
    m_swsCtx = sws_getContext(
        m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
        outWidth, outHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx) {
        std::cerr << "VideoPlayer: Failed to create scaler" << std::endl;
        close();
        return false;
    }

    std::cerr << "VideoPlayer: Opened " << path
              << " (" << m_codecCtx->width << "x" << m_codecCtx->height
              << " -> " << outWidth << "x" << outHeight
              << " @ " << getFPS() << " fps)" << std::endl;

    return true;
}

const uint8_t* VideoPlayer::nextFrame() {
    if (m_finished) return nullptr;

    if (decodeNextFrame()) {
        return m_rgbBuffer.data();
    }

    m_finished = true;
    return nullptr;
}

bool VideoPlayer::decodeNextFrame() {
    while (true) {
        int ret = av_read_frame(m_formatCtx, m_packet);
        if (ret < 0) {
            // End of file or error - flush decoder
            avcodec_send_packet(m_codecCtx, nullptr);
            ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == 0) {
                sws_scale(m_swsCtx, m_frame->data, m_frame->linesize,
                          0, m_codecCtx->height,
                          m_rgbFrame->data, m_rgbFrame->linesize);
                return true;
            }
            return false;
        }

        if (m_packet->stream_index == m_audioStreamIdx && m_audioCallback) {
            m_audioCallback(m_packet);
            av_packet_unref(m_packet);
            continue;
        } else if (m_packet->stream_index != m_videoStreamIdx) {
            av_packet_unref(m_packet);
            continue;
        }

        ret = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);

        if (ret < 0) continue;

        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) return false;

        // Scale to RGB24
        sws_scale(m_swsCtx, m_frame->data, m_frame->linesize,
                  0, m_codecCtx->height,
                  m_rgbFrame->data, m_rgbFrame->linesize);

        return true;
    }
}

void VideoPlayer::rewind() {
    if (m_formatCtx) {
        av_seek_frame(m_formatCtx, m_videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_codecCtx);
        m_finished = false;
    }
}

double VideoPlayer::getFPS() const {
    if (!m_formatCtx || m_videoStreamIdx < 0) return 25.0;

    AVRational fr = m_formatCtx->streams[m_videoStreamIdx]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        return (double)fr.num / (double)fr.den;
    }
    return 25.0;
}

AVCodecParameters* VideoPlayer::getAudioCodecPar() const {
    if (m_formatCtx && m_audioStreamIdx >= 0) {
        return m_formatCtx->streams[m_audioStreamIdx]->codecpar;
    }
    return nullptr;
}

AVRational VideoPlayer::getAudioTimeBase() const {
    if (m_formatCtx && m_audioStreamIdx >= 0) {
        return m_formatCtx->streams[m_audioStreamIdx]->time_base;
    }
    return {0, 1};
}

void VideoPlayer::close() {
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_rgbFrame) { av_frame_free(&m_rgbFrame); m_rgbFrame = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); m_packet = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); m_formatCtx = nullptr; }
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;
    m_audioCallback = nullptr;
    m_finished = true;
    m_rgbBuffer.clear();
}
