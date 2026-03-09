"""
BLE Pixel Panel Bridge Daemon.
Receives raw RGB frames via UDP from midi_ft_bridge (C++) and forwards
them to the iPixel Color BLE panel as PNG images.

Usage: python bt_bridge.py [--addr XX:XX:XX:XX:XX:XX] [--port 1340] [--brightness 80]

The C++ app sends a UDP packet per frame:
  - 4 bytes: width (uint16 LE) + height (uint16 LE)
  - width * height * 3 bytes: RGB24 pixel data
"""

import argparse
import asyncio
import binascii
import struct
import time
from io import BytesIO

from bleak import BleakClient, BleakScanner
from PIL import Image

# BLE protocol constants
WRITE_UUID = "0000fa02-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000fa03-0000-1000-8000-00805f9b34fb"
CHUNK_SIZE = 244
WINDOW_SIZE = 12 * 1024
ACK_TIMEOUT = 8.0

# Default device
DEFAULT_ADDR = "D2:DF:25:F1:E1:3D"
DEFAULT_PORT = 1340
DEFAULT_BRIGHTNESS = 80


def build_image_payload(png_bytes: bytes) -> list[list[bytes]]:
    """Build windowed + chunked BLE payload from PNG bytes."""
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
        length = len(window_data) + 2
        window_data = length.to_bytes(2, "little") + window_data

        chunks = []
        for i in range(0, len(window_data), CHUNK_SIZE):
            chunks.append(window_data[i:i + CHUNK_SIZE])

        windows.append(chunks)
        offset += WINDOW_SIZE

    return windows


class BLEBridge:
    def __init__(self, addr: str, udp_port: int, brightness: int):
        self.addr = addr
        self.udp_port = udp_port
        self.brightness = brightness
        self.client: BleakClient | None = None
        self.ack_event = asyncio.Event()
        self.connected = False

        # Stats
        self.frames_sent = 0
        self.frames_dropped = 0
        self.last_fps_time = time.monotonic()
        self.fps_count = 0
        self.current_fps = 0.0

        # Frame queue (latest frame only — drop old ones if BLE is slow)
        self.latest_frame: bytes | None = None
        self.frame_event = asyncio.Event()

    def _on_notify(self, _, data: bytes):
        if len(data) == 5 and data[0] == 0x05:
            self.ack_event.set()

    async def connect(self):
        """Connect to the BLE panel."""
        while True:
            try:
                print(f"Connecting to {self.addr}...")
                self.client = BleakClient(self.addr, timeout=15)
                await self.client.connect()
                await self.client.start_notify(NOTIFY_UUID, self._on_notify)

                # Set brightness
                cmd = bytes([5, 0, 4, 0x80, self.brightness])
                await self.client.write_gatt_char(WRITE_UUID, cmd, response=True)

                self.connected = True
                print(f"Connected. Brightness={self.brightness}")
                return
            except Exception as e:
                print(f"Connection failed: {e}. Retrying in 3s...")
                await asyncio.sleep(3)

    async def send_frame(self, rgb_data: bytes, width: int, height: int):
        """Convert RGB to PNG and send to BLE panel."""
        if not self.connected or not self.client:
            return

        try:
            # RGB -> PNG
            img = Image.frombytes("RGB", (width, height), rgb_data)
            buf = BytesIO()
            img.save(buf, format="PNG", optimize=False)
            png_bytes = buf.getvalue()

            # Build BLE payload and send
            windows = build_image_payload(png_bytes)
            for window_chunks in windows:
                self.ack_event.clear()
                for chunk in window_chunks:
                    await self.client.write_gatt_char(WRITE_UUID, chunk, response=True)

                try:
                    await asyncio.wait_for(self.ack_event.wait(), timeout=ACK_TIMEOUT)
                except asyncio.TimeoutError:
                    print(f"  ACK timeout (frame {self.frames_sent})")

            self.frames_sent += 1
            self.fps_count += 1

        except Exception as e:
            print(f"Send error: {e}")
            self.connected = False
            await self.reconnect()

    async def reconnect(self):
        """Reconnect after disconnect."""
        print("Disconnected. Reconnecting...")
        try:
            if self.client:
                await self.client.disconnect()
        except Exception:
            pass
        await self.connect()

    async def udp_receiver(self):
        """Receive RGB frames from C++ app via UDP."""
        loop = asyncio.get_event_loop()
        transport, protocol = await loop.create_datagram_endpoint(
            lambda: UDPFrameProtocol(self),
            local_addr=("127.0.0.1", self.udp_port),
        )
        print(f"Listening for RGB frames on UDP 127.0.0.1:{self.udp_port}")
        print(f"  Packet format: [uint16 width][uint16 height][RGB24 data]")

        # Keep running
        try:
            while True:
                await asyncio.sleep(3600)
        finally:
            transport.close()

    async def sender_loop(self):
        """Send latest frame to BLE, dropping old frames."""
        while True:
            await self.frame_event.wait()
            self.frame_event.clear()

            frame_data = self.latest_frame
            if frame_data is None:
                continue

            # Parse header
            if len(frame_data) < 4:
                continue
            width, height = struct.unpack("<HH", frame_data[:4])
            rgb = frame_data[4:]
            expected = width * height * 3
            if len(rgb) != expected:
                continue

            await self.send_frame(rgb, width, height)

            # FPS stats
            now = time.monotonic()
            if now - self.last_fps_time >= 5.0:
                self.current_fps = self.fps_count / (now - self.last_fps_time)
                print(f"  BLE: {self.current_fps:.1f} FPS  "
                      f"sent={self.frames_sent}  dropped={self.frames_dropped}")
                self.fps_count = 0
                self.last_fps_time = now

    async def run(self):
        await self.connect()
        await asyncio.gather(
            self.udp_receiver(),
            self.sender_loop(),
        )


class UDPFrameProtocol(asyncio.DatagramProtocol):
    def __init__(self, bridge: BLEBridge):
        self.bridge = bridge

    def datagram_received(self, data: bytes, addr):
        # Drop previous frame if BLE hasn't consumed it yet
        if self.bridge.latest_frame is not None:
            self.bridge.frames_dropped += 1
        self.bridge.latest_frame = data
        self.bridge.frame_event.set()


def main():
    parser = argparse.ArgumentParser(description="BLE Pixel Panel Bridge")
    parser.add_argument("--addr", default=DEFAULT_ADDR, help="BLE device address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP listen port")
    parser.add_argument("--brightness", type=int, default=DEFAULT_BRIGHTNESS, help="Panel brightness 0-100")
    args = parser.parse_args()

    print(f"BLE Bridge: {args.addr} | UDP port {args.port} | Brightness {args.brightness}")
    bridge = BLEBridge(args.addr, args.port, args.brightness)

    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
