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
    : m_demuxDone(false)
    , m_formatCtx(nullptr)
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
    m_demuxDone = false;
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

    return true;
}

void VideoPlayer::start() {
    // Starting threads is deferred until the caller has wired up the audio
    // callback. Demux reads packets, forwards audio directly to the audio
    // player, and pushes video packets into a bounded queue. Decode pulls
    // from that queue. Keeping the two decoupled is what prevents the frame
    // ring buffer from stalling audio playback.
    if (m_running) return;
    m_running = true;
    m_decodeThread = std::thread(&VideoPlayer::decodeThread, this);
    m_demuxThread = std::thread(&VideoPlayer::demuxThread, this);
}

void VideoPlayer::seek(double targetSec) {
    if (!m_formatCtx || !m_codecCtx || m_videoStreamIdx < 0) return;
    if (targetSec < 0) targetSec = 0;

    // 1. Stop the worker threads (race-free) without freeing ffmpeg state.
    stopThreads();

    // 2. Drop any packets left queued from before the seek.
    for (AVPacket* p : m_pktQueue) av_packet_free(&p);
    m_pktQueue.clear();

    // 3. Seek the container to the keyframe at/just before the target, then
    //    flush the decoder so it doesn't emit stale frames.
    int64_t ts = (int64_t)(targetSec * AV_TIME_BASE);
    av_seek_frame(m_formatCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codecCtx);

    // 4. Reset the frame ring and end-of-stream flags.
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        m_writeIdx = 0;
        m_readIdx = 0;
        m_writeCount = 0;
        m_readCount = 0;
        m_lastPTS = -1.0;
        for (int i = 0; i < FRAME_BUF_SIZE; i++) m_frameBuf[i].pts = -1.0;
    }
    m_finished = false;
    m_demuxDone = false;

    // 5. Restart the workers; they resume demuxing from the seek point.
    m_running = true;
    m_decodeThread = std::thread(&VideoPlayer::decodeThread, this);
    m_demuxThread = std::thread(&VideoPlayer::demuxThread, this);
}

void VideoPlayer::demuxThread() {
    // Reads packets from the container as fast as the packet queue allows.
    // Audio packets are handed to the audio callback immediately — this is
    // what keeps the SDL audio queue filled when the video frame ring is
    // full and the decode thread is parked in storeFrame().
    while (m_running) {
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(m_formatCtx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            {
                std::lock_guard<std::mutex> lock(m_pktMutex);
                m_demuxDone = true;
            }
            m_pktNotEmpty.notify_all();
            return;
        }

        if (pkt->stream_index == m_audioStreamIdx) {
            if (m_audioCallback) m_audioCallback(pkt);
            av_packet_free(&pkt);
            continue;
        }

        if (pkt->stream_index != m_videoStreamIdx) {
            av_packet_free(&pkt);
            continue;
        }

        // Video: push into the bounded packet queue (blocks if full).
        std::unique_lock<std::mutex> lock(m_pktMutex);
        m_pktNotFull.wait(lock, [&] {
            return !m_running || m_pktQueue.size() < PKT_QUEUE_MAX;
        });
        if (!m_running) {
            av_packet_free(&pkt);
            return;
        }
        m_pktQueue.push_back(pkt);
        lock.unlock();
        m_pktNotEmpty.notify_one();
    }
}

void VideoPlayer::decodeThread() {
    // Pulls video packets from the queue, decodes them into the frame ring
    // buffer. Paced by the consumer via storeFrame() blocking when full.
    auto drainDecoder = [&]() -> bool {
        while (m_running) {
            int ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
            if (ret < 0) return false;
            sws_scale(m_swsCtx, m_frame->data, m_frame->linesize,
                      0, m_codecCtx->height,
                      m_rgbFrame->data, m_rgbFrame->linesize);
            double pts = (m_frame->pts != AV_NOPTS_VALUE)
                ? m_frame->pts * m_videoTimeBase : -1.0;
            if (!storeFrame(pts)) return false;
        }
        return false;
    };

    while (m_running) {
        AVPacket* pkt = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_pktMutex);
            m_pktNotEmpty.wait(lock, [&] {
                return !m_running || !m_pktQueue.empty() || m_demuxDone;
            });
            if (!m_running) return;

            if (m_pktQueue.empty()) {
                // Demux finished — flush the decoder and exit.
                lock.unlock();
                avcodec_send_packet(m_codecCtx, nullptr);
                drainDecoder();
                m_finished = true;
                m_bufNotEmpty.notify_all();
                return;
            }

            pkt = m_pktQueue.front();
            m_pktQueue.pop_front();
        }
        m_pktNotFull.notify_one();

        int ret = avcodec_send_packet(m_codecCtx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) continue;

        if (!drainDecoder()) {
            m_finished = true;
            m_bufNotEmpty.notify_all();
            return;
        }
    }
}

bool VideoPlayer::storeFrame(double pts) {
    std::unique_lock<std::mutex> lock(m_bufMutex);

    m_bufNotFull.wait(lock, [&] {
        return !m_running || (m_writeCount - m_readCount) < FRAME_BUF_SIZE;
    });

    if (!m_running) return false;

    int slot = m_writeIdx % FRAME_BUF_SIZE;
    memcpy(m_frameBuf[slot].rgb.data(), m_rgbBuffer.data(),
           m_outWidth * m_outHeight * 3);
    m_frameBuf[slot].pts = pts;
    m_writeIdx = (m_writeIdx + 1) % FRAME_BUF_SIZE;
    m_writeCount++;

    lock.unlock();
    m_bufNotEmpty.notify_one();
    return true;
}

const uint8_t* VideoPlayer::getFrameAtTime(double timeSec) {
    // Audio-master sync: return the latest frame whose PTS <= timeSec, popping
    // any earlier frames we've fallen behind on. Returns nullptr when there is
    // no new frame to show (caller should keep displaying the previous one and
    // avoid re-sending to the panels).
    std::unique_lock<std::mutex> lock(m_bufMutex);

    // Drop stale frames: while the frame AFTER the front is also already due,
    // we're behind — discard the front and advance.
    while ((m_writeCount - m_readCount) >= 2) {
        int nextSlot = (m_readIdx + 1) % FRAME_BUF_SIZE;
        double nextPTS = m_frameBuf[nextSlot].pts;
        if (nextPTS >= 0 && nextPTS <= timeSec) {
            m_readIdx = (m_readIdx + 1) % FRAME_BUF_SIZE;
            m_readCount++;
        } else {
            break;
        }
    }

    int available = m_writeCount - m_readCount;
    if (available <= 0) return nullptr;

    int slot = m_readIdx % FRAME_BUF_SIZE;
    double frontPTS = m_frameBuf[slot].pts;

    // Frame still in the future — not time to show it yet.
    // (Unknown PTS (-1) is treated as "show now".)
    if (frontPTS >= 0 && frontPTS > timeSec) return nullptr;

    // Consume and return.
    memcpy(m_currentFrame.rgb.data(), m_frameBuf[slot].rgb.data(),
           m_outWidth * m_outHeight * 3);
    m_currentFrame.pts = frontPTS;
    m_lastPTS = frontPTS;
    m_readIdx = (m_readIdx + 1) % FRAME_BUF_SIZE;
    m_readCount++;

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

void VideoPlayer::stopThreads() {
    // Flip m_running while holding both queue mutexes. A waiter evaluates its
    // predicate under one of these mutexes, so this guarantees it either sees
    // m_running == false (and never waits) or is already suspended (and the
    // notify below reaches it). Setting the flag lock-free, as before, allowed
    // a lost wakeup that hung join() during clip transitions.
    {
        std::lock_guard<std::mutex> lockPkt(m_pktMutex);
        std::lock_guard<std::mutex> lockBuf(m_bufMutex);
        m_running = false;
    }
    m_pktNotFull.notify_all();
    m_pktNotEmpty.notify_all();
    m_bufNotFull.notify_all();
    m_bufNotEmpty.notify_all();

    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

void VideoPlayer::close() {
    stopThreads();

    // Drain any packets left in the queue.
    for (AVPacket* p : m_pktQueue) {
        av_packet_free(&p);
    }
    m_pktQueue.clear();
    m_demuxDone = false;

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
