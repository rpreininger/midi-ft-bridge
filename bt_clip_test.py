"""
Test: decode a 288x128 video, extract the BT region (32x16 at x=256,y=0),
and send it to bt_bridge.py via UDP — simulating what the C++ app does.

Usage:
  1. Start bt_bridge.py:  python bt_bridge.py
  2. Run this:            python bt_clip_test.py [clip_path] [max_fps]
"""

import socket
import struct
import sys
import time

import cv2

# BT region in the 288x128 video
SRC_X, SRC_Y = 256, 0
SRC_W, SRC_H = 32, 16
PORT = 1340
DEFAULT_MAX_FPS = 5


def main():
    clip_path = sys.argv[1] if len(sys.argv) > 1 else "media/Test1.mp4"
    max_fps = float(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MAX_FPS

    cap = cv2.VideoCapture(clip_path)
    if not cap.isOpened():
        print(f"Failed to open {clip_path}")
        return

    vid_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    vid_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    vid_fps = cap.get(cv2.CAP_PROP_FPS)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    print(f"Clip: {clip_path} ({vid_w}x{vid_h} @ {vid_fps:.1f} FPS, {total} frames)")
    print(f"BT region: x={SRC_X} y={SRC_Y} {SRC_W}x{SRC_H}")
    print(f"Sending at max {max_fps} FPS to UDP 127.0.0.1:{PORT}")
    print("Press Ctrl+C to stop\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    header = struct.pack("<HH", SRC_W, SRC_H)
    interval = 1.0 / max_fps

    sent = 0
    start = time.monotonic()

    try:
        while True:
            t0 = time.monotonic()
            ret, frame = cap.read()
            if not ret:
                # Loop the clip
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                ret, frame = cap.read()
                if not ret:
                    break

            # Extract BT region (OpenCV is BGR, convert to RGB)
            region = frame[SRC_Y:SRC_Y + SRC_H, SRC_X:SRC_X + SRC_W]
            region_rgb = cv2.cvtColor(region, cv2.COLOR_BGR2RGB)
            rgb_bytes = region_rgb.tobytes()

            sock.sendto(header + rgb_bytes, ("127.0.0.1", PORT))
            sent += 1

            if sent % 50 == 0:
                elapsed = time.monotonic() - start
                print(f"  Sent {sent} frames ({elapsed:.1f}s, {sent/elapsed:.1f} FPS avg)")

            # Pace to max_fps
            sleep_time = interval - (time.monotonic() - t0)
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        pass

    elapsed = time.monotonic() - start
    print(f"\nDone: {sent} frames in {elapsed:.1f}s ({sent/elapsed:.1f} FPS avg)")
    cap.release()
    sock.close()


if __name__ == "__main__":
    main()
