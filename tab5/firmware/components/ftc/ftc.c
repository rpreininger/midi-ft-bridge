#include "ftc.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// ---- reader ----------------------------------------------------------------

static int rd_seek(ftc_reader_t *r, uint64_t pos)
{
    if (r->fd < 0) {
        if (pos > r->mem_len) return -1;
        r->pos = pos;
        return 0;
    }
    // inside the current buffer window: just move head
    uint64_t win_start = r->pos - r->head;
    if (pos >= win_start && pos <= win_start + r->tail) {
        r->head = (size_t)(pos - win_start);
        r->pos = pos;
        return 0;
    }
    if (lseek(r->fd, (off_t)pos, SEEK_SET) < 0) return -1;
    r->head = r->tail = 0;
    r->pos = pos;
    return 0;
}

// Read exactly n bytes (or fewer at EOF). Big reads bypass the buffer.
static size_t rd_read(ftc_reader_t *r, void *dst, size_t n)
{
    uint8_t *d = dst;
    if (r->fd < 0) {
        uint64_t left = r->mem_len - r->pos;
        if (n > left) n = (size_t)left;
        memcpy(d, r->mem + r->pos, n);
        r->pos += n;
        return n;
    }
    size_t done = 0;
    while (done < n) {
        size_t avail = r->tail - r->head;
        if (avail) {
            size_t k = n - done < avail ? n - done : avail;
            memcpy(d + done, r->buf + r->head, k);
            r->head += k;
            done += k;
            continue;
        }
        // Always refill through r->buf: it is DMA-capable internal RAM, the
        // destination (frame buffers in PSRAM) is not, and the SD driver
        // degrades to sector-by-sector bounce copies for such targets.
        ssize_t got = read(r->fd, r->buf, r->cap);
        if (got <= 0) break;
        // buffer window starts at the current position
        r->head = 0;
        r->tail = (size_t)got;
    }
    // pos tracks bytes delivered from the buffer too
    // (direct reads already advanced it above)
    return done;
}

// rd_read advances pos for direct reads only; account buffered bytes here.
static size_t rd_get(ftc_reader_t *r, void *dst, size_t n)
{
    if (r->fd < 0) return rd_read(r, dst, n);
    uint64_t before_direct = r->pos;
    size_t head0 = r->head, tail0 = r->tail;
    (void)tail0;
    size_t got = rd_read(r, dst, n);
    // bytes taken from the buffer = got - (pos advanced by direct reads)
    size_t direct = (size_t)(r->pos - before_direct);
    r->pos += got - direct;
    (void)head0;
    return got;
}

static void rd_close(ftc_reader_t *r, void (*release)(void *))
{
    if (r->fd >= 0) close(r->fd);
    if (r->buf) release(r->buf);
    memset(r, 0, sizeof(*r));
    r->fd = -1;
}

// ---- clip ----------------------------------------------------------------

static int parse_and_alloc(ftc_clip_t *c, void *(*alloc)(size_t))
{
    uint8_t h[64];
    if (rd_get(&c->v, h, 64) != 64 || memcmp(h, "FTC1", 4) != 0) return -1;
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
    if (!c->hdr.width || !c->hdr.height || !c->hdr.fps_num || !c->hdr.fps_den) return -1;

    size_t fsz = (size_t)c->hdr.width * c->hdr.height * 3;
    c->frame = alloc(fsz);
    c->scratch = alloc(fsz);
    c->payload_cap = fsz + fsz / 255 + 64;  // LZ4 worst case
    c->payload = alloc(c->payload_cap);
    if (!c->frame || !c->scratch || !c->payload) return -1;
    memset(c->frame, 0, fsz);

    if (c->hdr.frame_count) {
        uint8_t first[4];
        if (rd_seek(&c->v, c->hdr.index_offset) || rd_get(&c->v, first, 4) != 4) return -1;
        if (rd_seek(&c->v, rd32(first))) return -1;
    }
    c->has_audio = c->hdr.audio_rate && c->hdr.audio_bytes;
    if (c->has_audio && rd_seek(&c->a, c->hdr.audio_offset)) return -1;
    return 0;
}

int ftc_open(ftc_clip_t *c, const char *path, void *(*alloc)(size_t), void *(*io_alloc)(size_t),
             size_t io_buf_size)
{
    memset(c, 0, sizeof(*c));
    c->v.fd = c->a.fd = -1;
    if (!alloc) alloc = malloc;
    if (!io_buf_size) io_buf_size = 64 * 1024;
    c->v.fd = open(path, O_RDONLY);
    c->a.fd = open(path, O_RDONLY);  // independent position for the audio stream
    if (c->v.fd < 0 || c->a.fd < 0) return -1;
    c->v.cap = c->a.cap = io_buf_size;
    if (!io_alloc) io_alloc = alloc;
    c->v.buf = io_alloc(io_buf_size);
    c->a.buf = io_alloc(io_buf_size);
    if (!c->v.buf || !c->a.buf) return -1;
    return parse_and_alloc(c, alloc);
}

int ftc_open_mem(ftc_clip_t *c, const void *data, size_t len, void *(*alloc)(size_t))
{
    memset(c, 0, sizeof(*c));
    c->v.fd = c->a.fd = -1;
    if (!alloc) alloc = malloc;
    c->v.mem = c->a.mem = data;
    c->v.mem_len = c->a.mem_len = len;
    return parse_and_alloc(c, alloc);
}

void ftc_close(ftc_clip_t *c, void (*release)(void *))
{
    if (!release) release = free;
    rd_close(&c->v, release);
    rd_close(&c->a, release);
    if (c->frame) release(c->frame);
    if (c->scratch) release(c->scratch);
    if (c->payload) release(c->payload);
    memset(c, 0, sizeof(*c));
    c->v.fd = c->a.fd = -1;
}

int ftc_decode_next(ftc_clip_t *c)
{
    if (c->next_frame >= c->hdr.frame_count) return 1;
    size_t fsz = (size_t)c->hdr.width * c->hdr.height * 3;

    // Records are 4-byte aligned.
    if (c->v.pos & 3) rd_seek(&c->v, (c->v.pos + 3) & ~(uint64_t)3);

    uint8_t fh[8];
    if (rd_get(&c->v, fh, 8) != 8) { c->errors++; return -1; }
    uint32_t len = rd32(fh);
    uint8_t type = fh[4];
    if (len > c->payload_cap) { c->errors++; return -2; }
    if (len && rd_get(&c->v, c->payload, len) != len) { c->errors++; return -1; }
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
    if (!c->has_audio) return 0;
    uint64_t left = c->hdr.audio_bytes - c->audio_pos;
    if (n > left) n = (size_t)left;
    size_t got = n ? rd_get(&c->a, dst, n) : 0;
    c->audio_pos += got;
    return got;
}
