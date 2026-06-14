// ====================================================================
//  Clip Player - Native macOS (AVFoundation + AudioToolbox) A/V player
//
//  Drop-in replacement for the FFmpeg/SDL2 clip_player.cpp on macOS.
//  No FFmpeg, no SDL2 — only system frameworks.
//
//    * Video: AVAssetReader pulls hardware-decoded frames as BGRA
//      CVPixelBuffers on a worker thread into a small PTS-indexed ring.
//    * Audio: a second AVAssetReader pulls LPCM (44.1k/stereo/float),
//      played through an AudioQueue. The AudioQueue's render sample-time
//      is the master clock (audio-master), exactly like the original
//      design; video frames are picked to match it.
//    * Silent clips fall back to a steady_clock wall clock.
//
//  AVAssetReader + AudioQueue are pull/callback based and run on their
//  own threads, so this works headless (no NSApplication / run loop),
//  unlike AVPlayer whose playback engine never reaches ReadyToPlay in a
//  plain CLI process.
// ====================================================================

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AudioToolbox/AudioToolbox.h>

#include "clip_player.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace {
constexpr int    OUT_RATE      = 44100;
constexpr int    OUT_CH        = 2;
constexpr int    AQ_NUM_BUFS   = 3;
constexpr int    AQ_BUF_FRAMES = 4096;
constexpr int    FRAME_RING    = 16;          // decoded video frames
constexpr size_t PCM_RING_CAP  = OUT_RATE * OUT_CH * 2;  // ~2s of samples
}

// BGRA CVPixelBuffer -> tightly packed RGB24, nearest-neighbour scaled to
// outW x outH (clips are usually already canvas-sized, so this is a copy).
static void convertBGRAtoRGB24(CVPixelBufferRef pb, uint8_t* dst, int outW, int outH) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(pb);
    const size_t   stride = CVPixelBufferGetBytesPerRow(pb);
    const int      srcW = (int)CVPixelBufferGetWidth(pb);
    const int      srcH = (int)CVPixelBufferGetHeight(pb);
    if (base && srcW > 0 && srcH > 0) {
        for (int y = 0; y < outH; ++y) {
            int sy = (int)((long long)y * srcH / outH);
            const uint8_t* srcRow = base + (size_t)sy * stride;
            uint8_t* dstRow = dst + (size_t)y * outW * 3;
            for (int x = 0; x < outW; ++x) {
                int sx = (int)((long long)x * srcW / outW);
                const uint8_t* px = srcRow + (size_t)sx * 4;  // BGRA
                dstRow[x * 3 + 0] = px[2];
                dstRow[x * 3 + 1] = px[1];
                dstRow[x * 3 + 2] = px[0];
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

// --------------------------------------------------------------------
//  Pimpl (compiled with -fobjc-arc).
// --------------------------------------------------------------------
struct ClipPlayer::Impl {
    AVURLAsset* asset = nil;

    int    outW = 0, outH = 0;
    double fps = 25.0;
    double duration = 0.0;
    bool   hasAudio = false;

    // --- video decode -> RGB24 ring ---
    AVAssetReader*            vReader = nil;
    AVAssetReaderTrackOutput* vOut = nil;
    AVAssetTrack*             vTrack = nil;
    std::thread               vThread;
    struct Frame { std::vector<uint8_t> rgb; double pts = -1.0; };
    Frame  ring[FRAME_RING];
    int    wr = 0, rd = 0;
    std::atomic<int> wrCount{0}, rdCount{0};
    std::mutex ringMtx;
    std::condition_variable ringNotFull, ringNotEmpty;
    std::atomic<bool> videoEOF{false};
    std::vector<uint8_t> current;  // last frame handed back (stable)

    // --- audio decode -> PCM ring -> AudioQueue ---
    AVAssetReader*            aReader = nil;
    AVAssetReaderTrackOutput* aOut = nil;
    AVAssetTrack*             aTrack = nil;
    std::thread               aThread;
    std::vector<float> pcm;        // ring of interleaved float samples
    size_t pcmRd = 0, pcmWr = 0, pcmAvail = 0;
    std::mutex pcmMtx;
    std::condition_variable pcmNotFull;
    std::atomic<bool> audioEOF{false};
    AudioQueueRef aq = nullptr;
    AudioQueueBufferRef aqBufs[AQ_NUM_BUFS] = {};
    std::atomic<double> clockBase{0.0};
    std::atomic<double> lastPos{0.0};

    // --- wall clock fallback (silent clips) ---
    std::chrono::steady_clock::time_point wallStart;
    std::chrono::steady_clock::time_point pauseStart;

    std::atomic<bool> running{false};
    std::atomic<bool> active{false};

    // ---- audio ring helpers ----
    size_t pcmPull(float* dst, size_t frames) {
        std::unique_lock<std::mutex> lk(pcmMtx);
        size_t want = frames * OUT_CH;
        size_t got = std::min(want, pcmAvail);
        for (size_t i = 0; i < got; ++i) { dst[i] = pcm[pcmRd]; pcmRd = (pcmRd + 1) % pcm.size(); }
        pcmAvail -= got;
        lk.unlock();
        pcmNotFull.notify_one();
        for (size_t i = got; i < want; ++i) dst[i] = 0.0f;  // pad with silence
        return got / OUT_CH;
    }
    void pcmPush(const float* src, size_t n) {  // n = sample count (incl channels)
        std::unique_lock<std::mutex> lk(pcmMtx);
        size_t i = 0;
        while (i < n && running.load()) {
            if (pcmAvail >= pcm.size()) {
                pcmNotFull.wait(lk, [&]{ return !running.load() || pcmAvail < pcm.size(); });
                if (!running.load()) return;
            }
            size_t chunk = std::min(n - i, pcm.size() - pcmAvail);
            for (size_t k = 0; k < chunk; ++k) { pcm[pcmWr] = src[i + k]; pcmWr = (pcmWr + 1) % pcm.size(); }
            pcmAvail += chunk; i += chunk;
        }
    }

    void videoLoop();
    void audioLoop();
    bool setupPipeline(double startSec, bool startPaused);
    void teardownPipeline();

    // AudioQueue render callback — pulls from the PCM ring (silence-padded so
    // the queue's sample clock keeps advancing past underruns / EOF).
    static void aqRender(void* user, AudioQueueRef q, AudioQueueBufferRef buf) {
        auto* d = static_cast<Impl*>(user);
        int frames = (int)(buf->mAudioDataByteSize / (OUT_CH * sizeof(float)));
        if (frames <= 0) frames = AQ_BUF_FRAMES;
        d->pcmPull((float*)buf->mAudioData, frames);
        buf->mAudioDataByteSize = frames * OUT_CH * sizeof(float);
        AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    }
};

void ClipPlayer::Impl::videoLoop() {
    while (running.load()) {
        CMSampleBufferRef sb = [vOut copyNextSampleBuffer];
        if (!sb) { videoEOF.store(true); ringNotEmpty.notify_one(); break; }
        double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
        CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
        if (img) {
            std::unique_lock<std::mutex> lk(ringMtx);
            ringNotFull.wait(lk, [&]{ return !running.load() || (wrCount - rdCount) < FRAME_RING; });
            if (!running.load()) { CFRelease(sb); break; }
            int slot = wr % FRAME_RING;
            if ((int)ring[slot].rgb.size() != outW * outH * 3) ring[slot].rgb.resize(outW * outH * 3);
            convertBGRAtoRGB24(img, ring[slot].rgb.data(), outW, outH);
            ring[slot].pts = pts;
            wr = (wr + 1) % FRAME_RING;
            wrCount++;
            lk.unlock();
            ringNotEmpty.notify_one();
        }
        CFRelease(sb);
    }
}

void ClipPlayer::Impl::audioLoop() {
    while (running.load()) {
        CMSampleBufferRef sb = [aOut copyNextSampleBuffer];
        if (!sb) { audioEOF.store(true); break; }
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        if (bb) {
            size_t len = 0; char* ptr = nullptr;
            if (CMBlockBufferGetDataPointer(bb, 0, nullptr, &len, &ptr) == kCMBlockBufferNoErr && ptr && len) {
                pcmPush((const float*)ptr, len / sizeof(float));
            }
        }
        CFRelease(sb);
    }
}

bool ClipPlayer::Impl::setupPipeline(double startSec, bool startPaused) {
    NSError* err = nil;
    CMTime start = CMTimeMakeWithSeconds(startSec, 600);
    CMTimeRange range = CMTimeRangeMake(start, kCMTimePositiveInfinity);

    // Video reader
    vReader = [[AVAssetReader alloc] initWithAsset:asset error:&err];
    if (!vReader) return false;
    if (startSec > 0.0) vReader.timeRange = range;
    NSDictionary* vs = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) };
    vOut = [[AVAssetReaderTrackOutput alloc] initWithTrack:vTrack outputSettings:vs];
    vOut.alwaysCopiesSampleData = NO;
    [vReader addOutput:vOut];
    if (![vReader startReading]) return false;

    // Audio reader + AudioQueue
    if (hasAudio && aTrack) {
        aReader = [[AVAssetReader alloc] initWithAsset:asset error:&err];
        if (aReader) {
            if (startSec > 0.0) aReader.timeRange = range;
            NSDictionary* as = @{
                AVFormatIDKey           : @(kAudioFormatLinearPCM),
                AVSampleRateKey         : @(OUT_RATE),
                AVNumberOfChannelsKey   : @(OUT_CH),
                AVLinearPCMBitDepthKey  : @32,
                AVLinearPCMIsFloatKey   : @YES,
                AVLinearPCMIsBigEndianKey: @NO,
                AVLinearPCMIsNonInterleaved: @NO,
            };
            aOut = [[AVAssetReaderTrackOutput alloc] initWithTrack:aTrack outputSettings:as];
            aOut.alwaysCopiesSampleData = NO;
            [aReader addOutput:aOut];
            if (![aReader startReading]) { aReader = nil; aOut = nil; hasAudio = false; }
        } else { hasAudio = false; }
    }

    running.store(true);
    videoEOF.store(false);
    audioEOF.store(false);
    clockBase.store(startSec);
    lastPos.store(startSec);

    vThread = std::thread(&Impl::videoLoop, this);

    if (hasAudio) {
        aThread = std::thread(&Impl::audioLoop, this);

        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate = OUT_RATE;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mChannelsPerFrame = OUT_CH;
        fmt.mBitsPerChannel = 32;
        fmt.mBytesPerFrame = OUT_CH * sizeof(float);
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerPacket = fmt.mBytesPerFrame;
        if (AudioQueueNewOutput(&fmt, Impl::aqRender, this, nullptr, nullptr, 0, &aq) == noErr) {
            // Give the decoder a moment to prime the ring, then fill + enqueue.
            for (int i = 0; i < 50; ++i) {
                { std::lock_guard<std::mutex> lk(pcmMtx); if (pcmAvail >= (size_t)AQ_BUF_FRAMES * OUT_CH) break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            for (int i = 0; i < AQ_NUM_BUFS; ++i) {
                AudioQueueAllocateBuffer(aq, AQ_BUF_FRAMES * OUT_CH * sizeof(float), &aqBufs[i]);
                aqBufs[i]->mAudioDataByteSize = AQ_BUF_FRAMES * OUT_CH * sizeof(float);
                Impl::aqRender(this, aq, aqBufs[i]);
            }
            if (!startPaused) AudioQueueStart(aq, nullptr);
        } else {
            aq = nullptr; hasAudio = false;
        }
    }

    // Wall-clock fallback origin (used for silent clips).
    auto now = std::chrono::steady_clock::now();
    wallStart = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(startSec));
    if (startPaused) pauseStart = now;
    return true;
}

void ClipPlayer::Impl::teardownPipeline() {
    running.store(false);
    [vReader cancelReading];
    [aReader cancelReading];
    ringNotFull.notify_all();
    ringNotEmpty.notify_all();
    pcmNotFull.notify_all();
    if (vThread.joinable()) vThread.join();
    if (aThread.joinable()) aThread.join();

    if (aq) { AudioQueueStop(aq, true); AudioQueueDispose(aq, true); aq = nullptr; }
    for (auto& b : aqBufs) b = nullptr;

    vReader = nil; vOut = nil;
    aReader = nil; aOut = nil;

    wr = rd = 0; wrCount.store(0); rdCount.store(0);
    { std::lock_guard<std::mutex> lk(pcmMtx); pcmRd = pcmWr = pcmAvail = 0; }
}

// ---- public surface ----

ClipPlayer::ClipPlayer() = default;
ClipPlayer::~ClipPlayer() { stop(); }

bool ClipPlayer::open(const std::string& clipPath, int videoWidth, int videoHeight,
                      AudioPlayer* audioPlayer) {
    (void)audioPlayer;  // audio handled natively by AudioQueue
    stop();

    m_impl = std::make_unique<Impl>();
    Impl& d = *m_impl;
    d.outW = videoWidth;
    d.outH = videoHeight;
    d.current.assign((size_t)videoWidth * videoHeight * 3, 0);
    d.pcm.assign(PCM_RING_CAP, 0.0f);

    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:clipPath.c_str()]];
        d.asset = [AVURLAsset URLAssetWithURL:url options:nil];

        __block AVAssetTrack* vt = nil; __block AVAssetTrack* at = nil;
        __block double dur = 0.0;
        dispatch_group_t grp = dispatch_group_create();
        dispatch_group_enter(grp);
        [d.asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*){
            vt = t.firstObject; dispatch_group_leave(grp);
        }];
        dispatch_group_enter(grp);
        [d.asset loadTracksWithMediaType:AVMediaTypeAudio completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*){
            at = t.firstObject; dispatch_group_leave(grp);
        }];
        dispatch_group_enter(grp);
        [d.asset loadValuesAsynchronouslyForKeys:@[@"duration"] completionHandler:^{
            dur = CMTimeGetSeconds(d.asset.duration); dispatch_group_leave(grp);
        }];
        if (dispatch_group_wait(grp, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8.0 * NSEC_PER_SEC))) != 0) {
            std::cerr << "ClipPlayer(macOS): timed out loading " << clipPath << std::endl;
            m_impl.reset(); return false;
        }
        if (!vt) {
            std::cerr << "ClipPlayer(macOS): no video track in " << clipPath << std::endl;
            m_impl.reset(); return false;
        }
        d.vTrack = vt;
        d.aTrack = at;
        d.hasAudio = (at != nil);
        d.fps = vt.nominalFrameRate; if (d.fps <= 0.0) d.fps = 25.0;
        d.duration = (dur > 0.0 && !isnan(dur)) ? dur : 0.0;

        if (!d.setupPipeline(0.0, /*startPaused*/false)) {
            std::cerr << "ClipPlayer(macOS): failed to start AVAssetReader for " << clipPath << std::endl;
            d.teardownPipeline(); m_impl.reset(); return false;
        }
        d.active.store(true);
    }

    std::cerr << "ClipPlayer(macOS): AVAssetReader playback ("
              << (m_impl->hasAudio ? "audio-master" : "silent/wall-clock") << ", "
              << m_impl->fps << " fps)" << std::endl;
    return true;
}

double ClipPlayer::getPosition() const {
    if (!m_impl || !m_impl->active.load()) return 0.0;
    Impl& d = *m_impl;
    if (d.hasAudio && d.aq && !m_paused.load()) {
        AudioTimeStamp ts = {};
        if (AudioQueueGetCurrentTime(d.aq, nullptr, &ts, nullptr) == noErr &&
            (ts.mFlags & kAudioTimeStampSampleTimeValid)) {
            double pos = d.clockBase.load() + ts.mSampleTime / (double)OUT_RATE;
            d.lastPos.store(pos);
            return pos;
        }
        return d.lastPos.load();
    }
    if (d.hasAudio) return d.lastPos.load();  // paused: hold
    // Silent: wall clock.
    auto ref = m_paused.load() ? d.pauseStart : std::chrono::steady_clock::now();
    return std::chrono::duration<double>(ref - d.wallStart).count();
}

const uint8_t* ClipPlayer::getCurrentFrame() {
    if (!m_impl || !m_impl->active.load()) return nullptr;
    Impl& d = *m_impl;

    double clock = getPosition();
    std::unique_lock<std::mutex> lk(d.ringMtx);

    // Drop frames we've fallen behind on (audio-master).
    while ((d.wrCount - d.rdCount) >= 2) {
        int next = (d.rd + 1) % FRAME_RING;
        double npts = d.ring[next].pts;
        if (npts >= 0 && npts <= clock) { d.rd = (d.rd + 1) % FRAME_RING; d.rdCount++; }
        else break;
    }

    int avail = d.wrCount - d.rdCount;
    if (avail <= 0) {
        if (d.videoEOF.load()) d.active.store(false);  // truly finished
        return nullptr;
    }
    int slot = d.rd % FRAME_RING;
    double pts = d.ring[slot].pts;
    if (pts >= 0 && pts > clock) return nullptr;  // not due yet — keep previous

    memcpy(d.current.data(), d.ring[slot].rgb.data(), (size_t)d.outW * d.outH * 3);
    d.rd = (d.rd + 1) % FRAME_RING;
    d.rdCount++;
    lk.unlock();
    d.ringNotFull.notify_one();
    return d.current.data();
}

void ClipPlayer::stop() {
    if (!m_impl) { m_paused = false; return; }
    m_impl->active.store(false);
    @autoreleasepool { m_impl->teardownPipeline(); }
    m_impl.reset();
    m_paused = false;
}

void ClipPlayer::pause() {
    if (!m_impl || m_paused) return;
    Impl& d = *m_impl;
    if (d.hasAudio && d.aq) { getPosition(); AudioQueuePause(d.aq); }
    else d.pauseStart = std::chrono::steady_clock::now();
    m_paused = true;
}

void ClipPlayer::resume() {
    if (!m_impl || !m_paused) return;
    Impl& d = *m_impl;
    if (d.hasAudio && d.aq) { AudioQueueStart(d.aq, nullptr); }
    else d.wallStart += (std::chrono::steady_clock::now() - d.pauseStart);
    m_paused = false;
}

bool ClipPlayer::isFinished() const { return !m_impl || !m_impl->active.load(); }
bool ClipPlayer::hasAudio() const { return m_impl && m_impl->hasAudio; }
double ClipPlayer::getFPS() const { return m_impl ? m_impl->fps : 25.0; }
double ClipPlayer::getDuration() const { return m_impl ? m_impl->duration : 0.0; }

void ClipPlayer::seek(double targetSec) {
    if (!m_impl || !m_impl->active.load()) return;
    Impl& d = *m_impl;
    double dur = d.duration;
    if (targetSec < 0) targetSec = 0;
    if (dur > 0.0 && targetSec > dur - 0.1) targetSec = dur - 0.1;
    @autoreleasepool {
        d.teardownPipeline();
        d.setupPipeline(targetSec, /*startPaused*/ m_paused.load());
    }
}
