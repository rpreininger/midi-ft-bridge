import socket, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.20"
port = 1337
w, h = 128, 64

# Solid red frame
pixels = bytes([255, 0, 0] * w * h)
header = f"P6\n{w} {h}\n255\n".encode()
packet = header + pixels

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(50):
    sock.sendto(packet, (host, port))
    time.sleep(0.04)
print(f"Sent 50 red frames ({len(packet)} bytes each) to {host}:{port}")
sock.close()
