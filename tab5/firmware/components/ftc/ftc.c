#include "ftc.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int ftc_lz4_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    const uint8_t *ip = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_cap;

    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= iend) break;  // last sequence has literals only

        if (iend - ip < 2) return -1;
        size_t off = ip[0] | ip[1] << 8;
        ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return -1;
        size_t mlen = token & 15;
        if (mlen == 15) {
            unsigned b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                mlen += b;
            } while (b == 255);
        }
        mlen += 4;
        if ((size_t)(oend - op) < mlen) return -1;
        const uint8_t *m = op - off;
        if (off >= mlen) {
            memcpy(op, m, mlen);
            op += mlen;
        } else if (off == 1) {
            memset(op, *m, mlen);
            op += mlen;
        } else {
            while (mlen--) *op++ = *m++;  // overlapping copy
        }
    }
    return (int)(op - dst);
}

int ftc_open(ftc_clip_t *c, const char *path, void *(*alloc)(size_t), size_t io_buf_size)
{
    memset(c, 0, sizeof(*c));
    if (!alloc) alloc = malloc;
    uint8_t h[64];
    c->vf = fopen(path, "rb");
    if (!c->vf) return -1;
    if (fread(h, 1, 64, c->vf) != 64 || memcmp(h, "FTC1", 4) != 0) goto fail;
    c->hdr.width = rd16(h + 4);
    c->hdr.height = rd16(h + 6);
    c->hdr.fps_num = rd16(h + 8);
    c->hdr.fps_den = rd16(h + 10);
    c->hdr.frame_count = rd32(h + 12);
    c->hdr.audio_rate = rd32(h + 16);
    c->hdr.audio_channels = rd16(h + 20);
    c->hdr.audio_bits = rd16(h + 22);
    c->hdr.audio_offset = rd64(h + 24);
    c->hdr.audio_bytes = rd64(h + 32);
    c->hdr.index_offset = rd64(h + 40);
    c->hdr.keyframe_interval = rd16(h + 48);
    if (!c->hdr.width || !c->hdr.height || !c->hdr.fps_num || !c->hdr.fps_den) goto fail;

    size_t fsz = (size_t)c->hdr.width * c->hdr.height * 3;
    c->frame = alloc(fsz);
    c->scratch = alloc(fsz);
    c->payload_cap = fsz + fsz / 255 + 64;  // LZ4 worst case
    c->payload = alloc(c->payload_cap);
    if (!c->frame || !c->scratch || !c->payload) goto fail;
    memset(c->frame, 0, fsz);

    // Position the video handle at frame 0 via the index (first entry).
    uint8_t first[4];
    if (c->hdr.frame_count) {
        if (fseek(c->vf, (long)c->hdr.index_offset, SEEK_SET) || fread(first, 1, 4, c->vf) != 4) goto fail;
        if (fseek(c->vf, (long)rd32(first), SEEK_SET)) goto fail;
    }
    if (io_buf_size) setvbuf(c->vf, NULL, _IOFBF, io_buf_size);

    if (c->hdr.audio_rate && c->hdr.audio_bytes) {
        c->af = fopen(path, "rb");
        if (!c->af) goto fail;
        if (io_buf_size) setvbuf(c->af, NULL, _IOFBF, io_buf_size);
        if (fseek(c->af, (long)c->hdr.audio_offset, SEEK_SET)) goto fail;
    }
    return 0;
fail:
    // Buffers are leaked to the caller's ftc_close(); files closed there too.
    return -1;
}

void ftc_close(ftc_clip_t *c, void (*release)(void *))
{
    if (!release) release = free;
    if (c->vf) fclose(c->vf);
    if (c->af) fclose(c->af);
    if (c->frame) release(c->frame);
    if (c->scratch) release(c->scratch);
    if (c->payload) release(c->payload);
    memset(c, 0, sizeof(*c));
}

int ftc_decode_next(ftc_clip_t *c)
{
    if (c->next_frame >= c->hdr.frame_count) return 1;
    size_t fsz = (size_t)c->hdr.width * c->hdr.height * 3;

    // Records are 4-byte aligned.
    long pos = ftell(c->vf);
    if (pos & 3) fseek(c->vf, 4 - (pos & 3), SEEK_CUR);

    uint8_t fh[8];
    if (fread(fh, 1, 8, c->vf) != 8) { c->errors++; return -1; }
    uint32_t len = rd32(fh);
    uint8_t type = fh[4];
    if (len > c->payload_cap) { c->errors++; return -2; }
    if (len && fread(c->payload, 1, len, c->vf) != len) { c->errors++; return -1; }
    c->next_frame++;

    switch (type) {
    case 0:
        if (ftc_lz4_decode(c->payload, len, c->frame, fsz) != (int)fsz) { c->errors++; return -3; }
        c->key_frames++;
        break;
    case 1: {
        if (ftc_lz4_decode(c->payload, len, c->scratch, fsz) != (int)fsz) { c->errors++; return -3; }
        // XOR 4 bytes at a time; fsz is a multiple of 4 for any even width.
        uint32_t *d = (uint32_t *)c->frame;
        const uint32_t *s = (const uint32_t *)c->scratch;
        size_t i, n = fsz / 4;
        for (i = 0; i < n; i++) d[i] ^= s[i];
        for (i = n * 4; i < fsz; i++) c->frame[i] ^= c->scratch[i];
        c->delta_frames++;
        break;
    }
    case 2:
        c->repeat_frames++;
        break;
    default:
        c->errors++;
        return -4;
    }
    return 0;
}

int ftc_seek_forward(ftc_clip_t *c, uint32_t target)
{
    int rc = 0;
    while (c->next_frame < target && c->next_frame < c->hdr.frame_count) {
        rc = ftc_decode_next(c);
        if (rc > 0) break;
    }
    return rc;
}

size_t ftc_read_audio(ftc_clip_t *c, void *dst, size_t n)
{
    if (!c->af) return 0;
    uint64_t left = c->hdr.audio_bytes - c->audio_pos;
    if (n > left) n = (size_t)left;
    size_t got = n ? fread(dst, 1, n, c->af) : 0;
    c->audio_pos += got;
    return got;
}
