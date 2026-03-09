"""
BLE Pixel Panel FPS test.
Sends a running timer (MM:SS.d) to the iPixel Color panel and measures
actual achieved frame rate.

Usage: python bt_fps_test.py [target_fps]
Default target: 10 FPS
"""

import sys
import time
import asyncio
from io import BytesIO
from PIL import Image, ImageDraw, ImageFont
from bleak import BleakClient

ADDR = "D2:DF:25:F1:E1:3D"
WIDTH, HEIGHT = 32, 16
WRITE_UUID = "0000fa02-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000fa03-0000-1000-8000-00805f9b34fb"

# pypixelcolor send_image protocol constants
CHUNK_SIZE = 244
WINDOW_SIZE = 12 * 1024
ACK_TIMEOUT = 8.0


def render_frame(elapsed: float, fps: float, frame_num: int) -> bytes:
    """Render a timer frame, return PNG bytes."""
    import math
    img = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    draw = ImageDraw.Draw(img)

    try:
        font_big = ImageFont.truetype("arial.ttf", 12)
        font_small = ImageFont.truetype("arial.ttf", 8)
    except OSError:
        font_big = ImageFont.load_default()
        font_small = font_big

    # Timer text: SS.d (top line, centered)
    secs = elapsed % 60
    timer_text = f"{secs:04.1f}"
    bbox = draw.textbbox((0, 0), timer_text, font=font_big)
    tw = bbox[2] - bbox[0]
    x = (WIDTH - tw) // 2
    y = -2  # compensate for font ascent offset

    # Color cycles through hues so you can see each frame change
    hue_r = int(127 + 127 * math.sin(frame_num * 0.2))
    hue_g = int(127 + 127 * math.sin(frame_num * 0.2 + 2.1))
    hue_b = int(127 + 127 * math.sin(frame_num * 0.2 + 4.2))

    draw.text((x, y), timer_text, fill=(hue_r, hue_g, hue_b), font=font_big)

    # FPS counter (bottom line, centered)
    fps_text = f"{fps:.1f}fps"
    bbox = draw.textbbox((0, 0), fps_text, font=font_small)
    fw = bbox[2] - bbox[0]
    fh = bbox[3] - bbox[1]
    draw.text(((WIDTH - fw) // 2, HEIGHT - fh), fps_text, fill=(80, 80, 80), font=font_small)

    buf = BytesIO()
    img.save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def build_image_payload(png_bytes: bytes) -> list[list[bytes]]:
    """Build windowed + chunked BLE payload from PNG bytes (pypixelcolor protocol)."""
    import binascii
    crc = binascii.crc32(png_bytes) & 0xFFFFFFFF
    total_len = len(png_bytes)

    windows = []
    offset = 0
    first = True
    while offset < total_len:
        chunk_data = png_bytes[offset:offset + WINDOW_SIZE]
        option = 0x00 if first else 0x02
        first = False

        header = bytearray()
        header.append(0x02)  # PNG type
        header.append(0x00)
        header.append(option)
        header.extend(total_len.to_bytes(4, "little"))
        header.extend(crc.to_bytes(4, "little"))
        header.append(0x00)  # PNG format
        header.append(0x00)  # save slot 0

        window_data = bytes(header) + chunk_data
        # Prepend 2-byte LE length
        length = len(window_data) + 2
        window_data = length.to_bytes(2, "little") + window_data

        # Split into BLE chunks
        chunks = []
        for i in range(0, len(window_data), CHUNK_SIZE):
            chunks.append(window_data[i:i + CHUNK_SIZE])

        windows.append(chunks)
        offset += WINDOW_SIZE

    return windows


async def run_test(target_fps: float):
    print(f"Connecting to {ADDR}...")
    async with BleakClient(ADDR, timeout=15) as client:
        print(f"Connected. MTU={client.mtu_size}")
        print(f"Target: {target_fps} FPS | Press Ctrl+C to stop\n")

        # ACK handling
        ack_event = asyncio.Event()

        def on_notify(_, data: bytes):
            if len(data) == 5 and data[0] == 0x05:
                ack_event.set()

        await client.start_notify(NOTIFY_UUID, on_notify)

        frame_num = 0
        start_time = time.monotonic()
        fps_window_start = start_time
        fps_window_frames = 0
        current_fps = 0.0
        frame_interval = 1.0 / target_fps

        try:
            while True:
                frame_start = time.monotonic()
                elapsed = frame_start - start_time

                # Render
                png_bytes = render_frame(elapsed, current_fps, frame_num)
                windows = build_image_payload(png_bytes)

                # Send all windows
                for window_chunks in windows:
                    ack_event.clear()
                    for chunk in window_chunks:
                        await client.write_gatt_char(WRITE_UUID, chunk, response=True)

                    # Wait for ACK
                    try:
                        await asyncio.wait_for(ack_event.wait(), timeout=ACK_TIMEOUT)
                    except asyncio.TimeoutError:
                        print(f"  ACK timeout on frame {frame_num}")

                frame_num += 1
                fps_window_frames += 1

                # Update FPS every second
                now = time.monotonic()
                if now - fps_window_start >= 1.0:
                    current_fps = fps_window_frames / (now - fps_window_start)
                    print(f"  t={elapsed:6.1f}s  frame={frame_num:5d}  FPS={current_fps:.1f}  PNG={len(png_bytes)}B")
                    fps_window_start = now
                    fps_window_frames = 0

                # Pace to target FPS
                send_time = time.monotonic() - frame_start
                sleep_time = frame_interval - send_time
                if sleep_time > 0:
                    await asyncio.sleep(sleep_time)

        except KeyboardInterrupt:
            pass

        total_time = time.monotonic() - start_time
        avg_fps = frame_num / total_time if total_time > 0 else 0
        print(f"\n--- Results ---")
        print(f"Frames sent: {frame_num}")
        print(f"Total time:  {total_time:.1f}s")
        print(f"Average FPS: {avg_fps:.2f}")
        print(f"PNG size:    ~{len(png_bytes)} bytes per frame")

        await client.stop_notify(NOTIFY_UUID)


if __name__ == "__main__":
    target = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    asyncio.run(run_test(target))
