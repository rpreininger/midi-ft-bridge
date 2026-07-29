#!/usr/bin/env python3
# Receiver: count packets + detect loss by sequence number. Large RCVBUF so we
# measure link/adapter loss, not socket-overflow. Exits 5s after last packet.
import socket, time
PORT = 9999
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
s.bind(("0.0.0.0", PORT))
s.settimeout(6)
seen = set(); maxseq = -1
try:
    while True:
        data, _ = s.recvfrom(2048)
        if len(data) < 4:
            continue
        seq = int.from_bytes(data[:4], "big")
        seen.add(seq)
        if seq > maxseq:
            maxseq = seq
except socket.timeout:
    pass
expected = maxseq + 1 if maxseq >= 0 else 0
lost = expected - len(seen)
loss = (100.0 * lost / expected) if expected else 0.0
print(f"RESULT received={len(seen)} expected={expected} lost={lost} loss%={loss:.2f}")
