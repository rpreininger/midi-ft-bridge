// ====================================================================
//  Video Player - Threaded FFmpeg MP4 decoder with PTS-indexed frames
// ====================================================================

#include "video_player.h"
#include <iostream>
#include <cstring>

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
    , m_videoTimeBase(0)
    , m_writeIdx(0)
    , m_readIdx(0)
    , m_writeCount(0)
    , m_readCount(0)
    , m_lastPTS(-1.0)
    , m_running(false)
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
    m_writeIdx = 0;
    m_readIdx = 0;
    m_writeCount = 0;
    m_readCount = 0;
    m_lastPTS = -1.0;

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

    // Store video time base for PTS conversion
    AVRational tb = m_formatCtx->streams[m_videoStreamIdx]->time_base;
    m_videoTimeBase = (tb.den > 0) ? (double)tb.num / tb.den : 1.0 / 90000.0;

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
    m_codecCtx->thread_count = 2;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "VideoPlayer: Failed to open codec" << std::endl;
        close();
        return false;
    }

    // Allocate frames
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    // Allocate RGB buffer for sws_scale output
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

    // Pre-allocate frame buffer slots
    size_t frameBytes = outWidth * outHeight * 3;
    for (int i = 0; i < FRAME_BUF_SIZE; i++) {
        m_frameBuf[i].rgb.resize(frameBytes);
        m_frameBuf[i].pts = -1.0;
    }
    m_currentFrame.rgb.resize(frameBytes);

    std::cerr << "VideoPlayer: Opened " << path
              << " (" << m_codecCtx->width << "x" << m_codecCtx->height
              << " -> " << outWidth << "x" << outHeight
              << " @ " << getFPS() << " fps)" << std::endl;

    // Start decode-ahead thread
    m_running = true;
    m_thread = std::thread(&VideoPlayer::decodeThread, this);

    return true;
}

void VideoPlayer::decodeThread() {
    while (m_running) {
        // Wait for space in the ring buffer
        {
            std::unique_lock<std::mutex> lock(m_bufMutex);
            m_bufNotFull.wait(lock, [&] {
                return !m_running || (m_writeCount - m_readCount) < FRAME_BUF_SIZE;
            });
        }

        if (!m_running) break;

        // Demux and decode one frame
        if (!decodeOneFrame()) {
            m_finished = true;
            m_bufNotEmpty.notify_all();
            break;
        }
    }
}

bool VideoPlayer::decodeOneFrame() {
    while (m_running) {
        int ret = av_read_frame(m_formatCtx, m_packet);
        if (ret < 0) {
            // EOF — flush the decoder
            avcodec_send_packet(m_codecCtx, nullptr);
            ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == 0) {
                sws_scale(m_swsCtx, m_frame->data, m_frame->linesize,
                          0, m_codecCtx->height,
                          m_rgbFrame->data, m_rgbFrame->linesize);

                double pts = (m_frame->pts != AV_NOPTS_VALUE)
                    ? m_frame->pts * m_videoTimeBase : -1.0;

                int slot = m_writeIdx % FRAME_BUF_SIZE;
                {
                    std::lock_guard<std::mutex> lock(m_bufMutex);
                    memcpy(m_frameBuf[slot].rgb.data(), m_rgbBuffer.data(),
                           m_outWidth * m_outHeight * 3);
                    m_frameBuf[slot].pts = pts;
                    m_writeIdx = (m_writeIdx + 1) % FRAME_BUF_SIZE;
                    m_writeCount++;
                }
                m_bufNotEmpty.notify_one();
                return true;
            }
            return false;  // truly done
        }

        // Route audio packets to the callback
        if (m_packet->stream_index == m_audioStreamIdx && m_audioCallback) {
            m_audioCallback(m_packet);
            av_packet_unref(m_packet);
            continue;
        } else if (m_packet->stream_index != m_videoStreamIdx) {
            av_packet_unref(m_packet);
            continue;
        }

        // Decode video packet
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

        double pts = (m_frame->pts != AV_NOPTS_VALUE)
            ? m_frame->pts * m_videoTimeBase : -1.0;

        // Write to ring buffer
        int slot = m_writeIdx % FRAME_BUF_SIZE;
        {
            std::lock_guard<std::mutex> lock(m_bufMutex);
            memcpy(m_frameBuf[slot].rgb.data(), m_rgbBuffer.data(),
                   m_outWidth * m_outHeight * 3);
            m_frameBuf[slot].pts = pts;
            m_writeIdx = (m_writeIdx + 1) % FRAME_BUF_SIZE;
            m_writeCount++;
        }
        m_bufNotEmpty.notify_one();
        return true;
    }
    return false;
}

const uint8_t* VideoPlayer::getFrameAtTime(double timeSec) {
    std::unique_lock<std::mutex> lock(m_bufMutex);

    int available = m_writeCount - m_readCount;
    if (available <= 0) {
        if (m_finished) return nullptr;
        // Wait briefly for a frame to become available
        m_bufNotEmpty.wait_for(lock, std::chrono::milliseconds(2), [&] {
            return (m_writeCount - m_readCount) > 0 || m_finished;
        });
        available = m_writeCount - m_readCount;
        if (available <= 0) return nullptr;
    }

    // Consume all frames up to and including the one closest to timeSec.
    // This drops frames that are too old (video was slower than audio).
    int bestSlot = m_readIdx;
    int consumed = 0;

    for (int i = 0; i < available; i++) {
        int slot = (m_readIdx + i) % FRAME_BUF_SIZE;
        double framePTS = m_frameBuf[slot].pts;

        // If this frame is still in the future, stop — use the previous one
        if (framePTS >= 0 && framePTS > timeSec + 0.001) {
            break;
        }

        bestSlot = slot;
        consumed = i + 1;
    }

    // If no frames were consumed (all are in the future), show the first available
    if (consumed == 0) {
        bestSlot = m_readIdx;
        consumed = 1;
    }

    // Copy the best frame to the stable output buffer
    memcpy(m_currentFrame.rgb.data(), m_frameBuf[bestSlot].rgb.data(),
           m_outWidth * m_outHeight * 3);
    m_currentFrame.pts = m_frameBuf[bestSlot].pts;
    m_lastPTS = m_currentFrame.pts;

    // Advance the read pointer, freeing ring buffer slots
    m_readIdx = (m_readIdx + consumed) % FRAME_BUF_SIZE;
    m_readCount += consumed;

    lock.unlock();
    m_bufNotFull.notify_one();

    return m_currentFrame.rgb.data();
}

double VideoPlayer::getFPS() const {
    if (!m_formatCtx || m_videoStreamIdx < 0) return 25.0;

    AVRational fr = m_formatCtx->streams[m_videoStreamIdx]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        return (double)fr.num / (double)fr.den;
    }
    return 25.0;
}

double VideoPlayer::getDuration() const {
    if (!m_formatCtx) return 0.0;
    if (m_formatCtx->duration > 0) {
        return (double)m_formatCtx->duration / AV_TIME_BASE;
    }
    return 0.0;
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
    m_running = false;
    m_bufNotFull.notify_all();
    m_bufNotEmpty.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }

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
