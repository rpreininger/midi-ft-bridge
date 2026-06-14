// Smoke test for the native (AVFoundation) ClipPlayer.
// Opens a clip, pulls frames for ~2s, and reports decode stats.
#include "clip_player.h"
#include "audio_player.h"
#include <cstdio>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "clips/mp4/Intro.mp4";
    const int W = 288, H = 128;

    AudioPlayer audio;
    audio.init("default");

    ClipPlayer clip;
    if (!clip.open(path, W, H, &audio)) {
        printf("FAIL: open(%s)\n", path);
        return 1;
    }
    printf("opened: fps=%.2f duration=%.2fs hasAudio=%d\n",
           clip.getFPS(), clip.getDuration(), clip.hasAudio());

    int frames = 0;
    long pixelSum = 0;
    double lastPos = 0.0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.0) {
        const uint8_t* f = clip.getCurrentFrame();
        if (f) {
            frames++;
            for (int i = 0; i < W * H * 3; i += 997) pixelSum += f[i];  // sparse sample
        }
        lastPos = clip.getPosition();
        if (clip.isFinished()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    printf("pulled %d new frames in 2s, position advanced to %.2fs, pixelSum=%ld\n",
           frames, lastPos, pixelSum);

    // Exercise seek + pause
    clip.seek(clip.getDuration() * 0.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printf("after seek to mid: position=%.2fs\n", clip.getPosition());

    clip.pause();
    double p1 = clip.getPosition();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double p2 = clip.getPosition();
    printf("paused: pos %.3f -> %.3f (delta=%.3f, should be ~0)\n", p1, p2, p2 - p1);
    clip.resume();

    bool ok = frames > 10 && lastPos > 0.5 && pixelSum > 0 && (p2 - p1) < 0.1;
    printf("%s\n", ok ? "PASS" : "FAIL");
    clip.stop();
    audio.close();
    return ok ? 0 : 1;
}
