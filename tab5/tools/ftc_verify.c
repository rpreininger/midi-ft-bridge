// Host-side check of the firmware's FTC1 decoder: decodes every frame and
// writes the RGB24 stream + PCM to files, so they can be diffed against what
// ffmpeg decodes from the source. Also times the decode.
//
//   cc -O2 -I../firmware/components/ftc/include ftc_verify.c ../firmware/components/ftc/ftc.c -o ftc_verify
//   ./ftc_verify clip.ftc out.rgb out.pcm
#include "ftc.h"
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s clip.ftc [out.rgb] [out.pcm]\n", argv[0]); return 2; }
    ftc_clip_t c;
    if (ftc_open(&c, argv[1], NULL, 32768)) { fprintf(stderr, "open failed\n"); ftc_close(&c, NULL); return 1; }
    FILE *ro = argc > 2 ? fopen(argv[2], "wb") : NULL;
    FILE *ao = argc > 3 ? fopen(argv[3], "wb") : NULL;
    size_t fsz = (size_t)c.hdr.width * c.hdr.height * 3;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc;
    while ((rc = ftc_decode_next(&c)) <= 0) {
        if (rc < 0) fprintf(stderr, "frame %u: error %d\n", c.next_frame, rc);
        if (ro) fwrite(c.frame, 1, fsz, ro);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    char buf[65536];
    size_t n, total = 0;
    while ((n = ftc_read_audio(&c, buf, sizeof buf)) > 0) { total += n; if (ao) fwrite(buf, 1, n, ao); }
    printf("%ux%u @%u/%u, %u frames (key %u delta %u repeat %u, errors %u), %.3f ms/frame, audio %zu bytes @%u Hz\n",
           c.hdr.width, c.hdr.height, c.hdr.fps_num, c.hdr.fps_den, c.hdr.frame_count,
           c.key_frames, c.delta_frames, c.repeat_frames, c.errors,
           s * 1000 / (c.hdr.frame_count ? c.hdr.frame_count : 1), total, c.hdr.audio_rate);
    if (ro) fclose(ro);
    if (ao) fclose(ao);
    int bad = c.errors != 0;
    ftc_close(&c, NULL);
    return bad;
}
