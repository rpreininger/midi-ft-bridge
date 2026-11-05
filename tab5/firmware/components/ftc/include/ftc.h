// FTC1 clip reader (see tab5/FORMAT.md). Plain C + stdio so the same file
// builds for the ESP32-P4 firmware and for the host-side test on the Mac.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width, height;
    uint16_t fps_num, fps_den;
    uint32_t frame_count;
    uint32_t audio_rate;
    uint16_t audio_channels, audio_bits;
    uint64_t audio_offset, audio_bytes;
    uint64_t index_offset;
    uint16_t keyframe_interval;
} ftc_header_t;

typedef struct {
    FILE *vf;               // video handle
    FILE *af;               // audio handle (NULL if no audio)
    ftc_header_t hdr;
    uint32_t next_frame;    // index of the frame decode_next() produces next
    uint64_t audio_pos;     // bytes of PCM already read
    uint8_t *frame;         // current RGB888 frame (width*height*3)
    uint8_t *scratch;       // decompressed delta
    uint8_t *payload;       // compressed payload read buffer
    size_t payload_cap;
    // stats
    uint32_t key_frames, delta_frames, repeat_frames, errors;
} ftc_clip_t;

// LZ4 block decoder. Returns bytes written to dst, or -1 on malformed input /
// output overflow. Never reads/writes out of bounds.
int ftc_lz4_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

// Opens path; allocs buffers with `alloc` (NULL = malloc). io_buf_size > 0
// gives each FILE a setvbuf buffer of that size. 0 on success.
int ftc_open(ftc_clip_t *c, const char *path, void *(*alloc)(size_t), size_t io_buf_size);
void ftc_close(ftc_clip_t *c, void (*release)(void *));

// Decode the next frame into c->frame. 0 ok, 1 end of clip, <0 error (the
// previous frame stays in c->frame, playback can continue).
int ftc_decode_next(ftc_clip_t *c);

// Skip forward so that next_frame == target (decodes intermediate deltas; a
// key frame on the way short-circuits). Used when video falls behind audio.
int ftc_seek_forward(ftc_clip_t *c, uint32_t target);

// Read up to n bytes of PCM. Returns bytes read (0 at end).
size_t ftc_read_audio(ftc_clip_t *c, void *dst, size_t n);

#ifdef __cplusplus
}
#endif
