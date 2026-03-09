import socket, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.20"
port = 1337

# Tiny 10x10 red frame - fits in single UDP packet (< 1500 bytes)
w, h = 10, 10
pixels = bytes([255, 0, 0] * w * h)
header = f"P6\n{w} {h}\n255\n".encode()
packet = header + pixels

print(f"Packet size: {len(packet)} bytes")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(100):
    sock.sendto(packet, (host, port))
    time.sleep(0.04)
print(f"Sent 100 tiny red frames to {host}:{port}")
sock.close()
