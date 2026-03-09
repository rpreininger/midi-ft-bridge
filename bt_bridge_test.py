"""
Test script that simulates the C++ app sending RGB frames to bt_bridge.py.
Sends a color-cycling animation at ~10 FPS to UDP port 1340.

Usage:
  1. Start bt_bridge.py:  python bt_bridge.py
  2. Run this test:        python bt_bridge_test.py
"""

import math
import socket
import struct
import time

WIDTH, HEIGHT = 32, 16
PORT = 1340
TARGET_FPS = 10


def render_rgb(frame_num: int) -> bytes:
    """Generate a simple color-cycling gradient."""
    pixels = bytearray(WIDTH * HEIGHT * 3)
    t = frame_num * 0.1

    for y in range(HEIGHT):
        for x in range(WIDTH):
            offset = (y * WIDTH + x) * 3
            r = int(127 + 127 * math.sin(t + x * 0.3))
            g = int(127 + 127 * math.sin(t + y * 0.4 + 2.0))
            b = int(127 + 127 * math.sin(t + (x + y) * 0.2 + 4.0))
            pixels[offset] = r
            pixels[offset + 1] = g
            pixels[offset + 2] = b

    return bytes(pixels)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    header = struct.pack("<HH", WIDTH, HEIGHT)
    interval = 1.0 / TARGET_FPS

    print(f"Sending {WIDTH}x{HEIGHT} RGB frames to UDP 127.0.0.1:{PORT} at {TARGET_FPS} FPS")
    print("Press Ctrl+C to stop")

    frame = 0
    start = time.monotonic()
    try:
        while True:
            t0 = time.monotonic()
            rgb = render_rgb(frame)
            sock.sendto(header + rgb, ("127.0.0.1", PORT))
            frame += 1

            if frame % (TARGET_FPS * 5) == 0:
                elapsed = time.monotonic() - start
                print(f"  Sent {frame} frames in {elapsed:.1f}s ({frame/elapsed:.1f} FPS)")

            sleep_time = interval - (time.monotonic() - t0)
            if sleep_time > 0:
                time.sleep(sleep_time)
    except KeyboardInterrupt:
        elapsed = time.monotonic() - start
        print(f"\nSent {frame} frames in {elapsed:.1f}s ({frame/elapsed:.1f} FPS avg)")

    sock.close()


if __name__ == "__main__":
    main()
