#!/usr/bin/env python3
# Faithful FT-burst generator: `burst` packets of `payload` bytes sent
# back-to-back every `interval`, mimicking bigpanel's 128x128 tile-mode frames.
# First 4 bytes = big-endian sequence number.
import socket, sys, time
ip       = sys.argv[1]
port     = int(sys.argv[2]) if len(sys.argv) > 2 else 9999
burst    = int(sys.argv[3]) if len(sys.argv) > 3 else 43      # packets/frame
interval = float(sys.argv[4]) if len(sys.argv) > 4 else 0.040 # 25 fps
duration = float(sys.argv[5]) if len(sys.argv) > 5 else 15.0
payload  = 1152                                               # 3 rows*128*3 bytes

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pad = bytes(payload - 4)
seq = 0
next_t = time.time()
end = next_t + duration
while time.time() < end:
    for _ in range(burst):
        s.sendto(seq.to_bytes(4, "big") + pad, (ip, port))
        seq += 1
    next_t += interval
    dt = next_t - time.time()
    if dt > 0:
        time.sleep(dt)
rate = seq / duration
print(f"sent={seq} pps~={rate:.0f} mbit~={rate*payload*8/1e6:.1f}")
