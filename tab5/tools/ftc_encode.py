#!/usr/bin/env python3
"""Transcode a clip into the FTC1 container for the Tab5 player (see ../FORMAT.md).

    python3 -m venv .venv && .venv/bin/pip install numpy lz4
    .venv/bin/python ftc_encode.py ../../clips/mp4/Fly.mp4 -o /Volumes/TAB5SD/clips/Fly.ftc
    .venv/bin/python ftc_encode.py ../../clips/mp4/*.mp4 -d /Volumes/TAB5SD/clips

Needs ffmpeg on PATH. Video is scaled (nearest neighbour) to --size and
resampled to --fps only if the source differs; audio becomes s16le stereo
48 kHz.
"""
import argparse, os, struct, subprocess, sys
import numpy as np
import lz4.block

HDR = struct.Struct('<4sHHHHIIHHQQQH14x')
assert HDR.size == 64
FRAME_HDR = struct.Struct('<IB3x')
KEY, DELTA, REPEAT = 0, 1, 2


def align(f, a):
    pad = (-f.tell()) % a
    if pad:
        f.write(b'\0' * pad)


def encode(src, dst, w, h, fps, rate, keyint, quiet=False, start=None, duration=None):
    trim = (['-ss', str(start)] if start else []) + (['-t', str(duration)] if duration else [])
    # Audio first: decode to a temp raw file next to dst so the container can
    # keep it contiguous ahead of the frames.
    tmp_audio = dst + '.pcm.tmp'
    r = subprocess.run(['ffmpeg', '-v', 'error', '-y', *trim, '-i', src, '-vn',
                        '-ac', '2', '-ar', str(rate), '-f', 's16le', tmp_audio])
    has_audio = r.returncode == 0 and os.path.getsize(tmp_audio) > 0

    vf = f'scale={w}:{h}:flags=neighbor,fps={fps}'
    proc = subprocess.Popen(['ffmpeg', '-v', 'error', *trim, '-i', src, '-an', '-vf', vf,
                             '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                            stdout=subprocess.PIPE)
    fsize = w * h * 3
    offsets = []
    stats = {KEY: 0, DELTA: 0, REPEAT: 0}
    payload_total = 0

    with open(dst + '.tmp', 'wb') as out:
        out.write(b'\0' * 64)
        align(out, 4096)
        audio_offset = out.tell()
        audio_bytes = 0
        if has_audio:
            with open(tmp_audio, 'rb') as a:
                while chunk := a.read(1 << 20):
                    out.write(chunk)
                    audio_bytes += len(chunk)
        os.unlink(tmp_audio) if os.path.exists(tmp_audio) else None
        align(out, 4096)

        prev = None
        n = 0
        while True:
            buf = proc.stdout.read(fsize)
            if len(buf) < fsize:
                break
            cur = np.frombuffer(buf, dtype=np.uint8)
            align(out, 4)
            offsets.append(out.tell())
            if prev is not None and n % keyint and np.array_equal(cur, prev):
                kind, payload = REPEAT, b''
            elif prev is None or n % keyint == 0:
                kind, payload = KEY, lz4.block.compress(buf, store_size=False)
            else:
                kind, payload = DELTA, lz4.block.compress((cur ^ prev).tobytes(), store_size=False)
            out.write(FRAME_HDR.pack(len(payload), kind))
            out.write(payload)
            stats[kind] += 1
            payload_total += len(payload)
            prev = cur
            n += 1
        if proc.wait() != 0:
            raise RuntimeError(f'ffmpeg video decode failed for {src}')

        align(out, 4)
        index_offset = out.tell()
        if offsets and offsets[-1] >= 1 << 32:
            raise RuntimeError('clip exceeds 4 GB, u32 index cannot address it')
        out.write(struct.pack(f'<{len(offsets)}I', *offsets))
        out.seek(0)
        out.write(HDR.pack(b'FTC1', w, h, fps, 1, n,
                           rate if has_audio else 0, 2, 16,
                           audio_offset, audio_bytes, index_offset, keyint))
    os.replace(dst + '.tmp', dst)

    if not quiet:
        secs = n / fps
        total = os.path.getsize(dst)
        print(f'{os.path.basename(dst)}: {n} frames ({secs:.1f}s), '
              f'key {stats[KEY]} delta {stats[DELTA]} repeat {stats[REPEAT]}, '
              f'video {payload_total / max(n, 1) / 1024:.1f} KB/frame '
              f'= {payload_total / max(secs, 1e-9) / 1e6:.2f} MB/s, '
              f'audio {audio_bytes / 1e6:.1f} MB, file {total / 1e6:.1f} MB')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src', nargs='+')
    ap.add_argument('-o', '--output', help='output file (single input only)')
    ap.add_argument('-d', '--outdir', help='output directory (name.ftc per input)')
    ap.add_argument('--size', default='288x128')
    ap.add_argument('--fps', type=int, default=25)
    ap.add_argument('--rate', type=int, default=48000)
    ap.add_argument('--keyint', type=int, default=50)
    ap.add_argument('--start', type=float, help='cut: start time in seconds')
    ap.add_argument('--duration', type=float, help='cut: length in seconds')
    a = ap.parse_args()
    w, h = map(int, a.size.lower().split('x'))
    if a.output and len(a.src) != 1:
        ap.error('-o needs exactly one input; use -d for several')
    for src in a.src:
        dst = a.output or os.path.join(a.outdir or os.path.dirname(src),
                                       os.path.splitext(os.path.basename(src))[0] + '.ftc')
        encode(src, dst, w, h, a.fps, a.rate, a.keyint, start=a.start, duration=a.duration)


if __name__ == '__main__':
    sys.exit(main())
