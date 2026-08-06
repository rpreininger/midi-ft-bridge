#!/usr/bin/env python3
"""
Flaschen-Taschen panel simulator — stands in for the three Pi panels.

Binds the panels' real addresses (from config.json), decodes the tiled PPM
stream that ft_sender.cpp emits, and renders every panel live in the terminal
with per-panel fps / bitrate / tile-loss. That makes a full rehearsal possible
with no panels on the bench, and — because it counts missing tiles — it also
measures what the WLAN is doing to the stream.

Wire format (see src/ft_sender.cpp): one UDP packet per 3-row strip,
    P6\\n#FT: <x> <y>\\n<w> <h>\\n255\\n<RGB bytes>
with <y> the row offset inside the panel. A packet with y == 0 starts a frame.

Usage
    # one-time: give this Mac the panels' addresses (needs sudo)
    setup/sim-panel-ips.sh en9 up

    setup/ft-panel-sim.py                  # read ./config.json, render
    setup/ft-panel-sim.py --stats-only     # numbers only, for long soaks
    setup/ft-panel-sim.py --bind-any       # ignore panel IPs, bind 0.0.0.0

Ctrl-C prints a summary.
"""
import argparse
import json
import re
import select
import shutil
import socket
import sys
import time

HEADER_RE = re.compile(rb"^P6\n#FT: (-?\d+) (-?\d+)\n(\d+) (\d+)\n255\n")

# Panel types that speak this protocol. "ble"/"ble_udp" are the BT pixel panel
# and use a different framing, so they are not simulated here.
FT_TYPES = (None, "", "ft", "udp")


class Panel:
    def __init__(self, name, ip, port, width, height, bind_any=False):
        self.name = name
        self.ip = ip
        self.port = port
        self.width = width
        self.height = height
        self.fb = bytearray(width * height * 3)

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Generous receive buffer: we want to measure the link, not our own
        # scheduling. The real panels' ft-server does the same.
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("0.0.0.0" if bind_any else ip, port))
        self.sock.setblocking(False)

        self.packets = 0
        self.bytes = 0
        self.frames = 0
        self.last_packet = 0.0
        self.tiles_this_frame = 0
        self.expected_tiles = (height + 2) // 3   # TILE_ROWS = 3
        self.missing_tiles = 0
        self.short_frames = 0
        self._window_start = time.monotonic()
        self._window_frames = 0
        self._window_bytes = 0
        self.fps = 0.0
        self.mbits = 0.0

    def fileno(self):
        return self.sock.fileno()

    def drain(self):
        """Read everything queued on this socket."""
        while True:
            try:
                data = self.sock.recv(65535)
            except BlockingIOError:
                return
            except OSError:
                return
            self.ingest(data)

    def ingest(self, data):
        m = HEADER_RE.match(data)
        if not m:
            return                       # not our framing (e.g. ble_udp raw)
        _x, y, w, h = (int(g) for g in m.groups())
        payload = data[m.end():]
        if w != self.width or len(payload) < w * h * 3:
            return

        self.packets += 1
        self.bytes += len(data)
        self.last_packet = time.monotonic()

        if y == 0:
            # A new frame starts. Whatever the previous one was missing never
            # arrived — that is real, on-the-wire loss.
            if self.tiles_this_frame:
                lost = self.expected_tiles - self.tiles_this_frame
                if lost > 0:
                    self.missing_tiles += lost
                    self.short_frames += 1
            self.frames += 1
            self._window_frames += 1
            self.tiles_this_frame = 0

        self.tiles_this_frame += 1
        row_bytes = w * 3
        for row in range(h):
            dst = (y + row) * row_bytes
            if 0 <= dst <= len(self.fb) - row_bytes:
                self.fb[dst:dst + row_bytes] = payload[row * row_bytes:(row + 1) * row_bytes]
        self._window_bytes += len(data)

    def tick_rates(self, now):
        elapsed = now - self._window_start
        if elapsed >= 1.0:
            self.fps = self._window_frames / elapsed
            self.mbits = (self._window_bytes * 8) / elapsed / 1e6
            self._window_start = now
            self._window_frames = 0
            self._window_bytes = 0

    @property
    def alive(self):
        return self.last_packet and (time.monotonic() - self.last_packet) < 2.0

    def status(self):
        state = "LIVE" if self.alive else "----"
        return (f"{state} {self.name:<12} {self.ip}:{self.port} "
                f"{self.width}x{self.height}  {self.fps:5.1f} fps  "
                f"{self.mbits:5.2f} Mbit/s  frames={self.frames}  "
                f"lost tiles={self.missing_tiles} ({self.short_frames} frames)")


def render(panel, scale):
    """Panel as ANSI truecolor half-blocks: one character = 2 pixels."""
    w, h = panel.width, panel.height
    step = max(1, scale)
    out = []
    for y in range(0, h - step, step * 2):
        line = []
        prev = None
        for x in range(0, w, step):
            top = pixel(panel.fb, w, x, y)
            bot = pixel(panel.fb, w, x, min(y + step, h - 1))
            if (top, bot) != prev:
                line.append(f"\x1b[38;2;{top[0]};{top[1]};{top[2]}m"
                            f"\x1b[48;2;{bot[0]};{bot[1]};{bot[2]}m")
                prev = (top, bot)
            line.append("▀")        # upper half block
        line.append("\x1b[0m")
        out.append("".join(line))
    return out


def pixel(fb, width, x, y):
    i = (y * width + x) * 3
    return fb[i], fb[i + 1], fb[i + 2]


def load_panels(config_path, bind_any):
    with open(config_path) as fh:
        cfg = json.load(fh)
    panels = []
    for p in cfg.get("panels", []):
        if p.get("type") not in FT_TYPES:
            continue
        panels.append(Panel(p.get("name", "?"),
                            p.get("ip", "0.0.0.0"),
                            int(p.get("port", 1337)),
                            int(p.get("src_w", 128)),
                            int(p.get("src_h", 128)),
                            bind_any=bind_any))
    return panels


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--bind-any", action="store_true",
                    help="bind 0.0.0.0 instead of each panel's own IP "
                         "(no interface aliases needed, but all panels must "
                         "then use distinct ports)")
    ap.add_argument("--stats-only", action="store_true",
                    help="no picture, just the counters")
    ap.add_argument("--scale", type=int, default=0,
                    help="pixel decimation; 0 = auto-fit the terminal")
    args = ap.parse_args()

    try:
        panels = load_panels(args.config, args.bind_any)
    except OSError as e:
        sys.exit(f"bind failed: {e}\n"
                 f"The panel IPs must exist on this Mac — run "
                 f"setup/sim-panel-ips.sh <iface> up first, or use --bind-any.")
    if not panels:
        sys.exit(f"no FT panels in {args.config}")

    for p in panels:
        print(f"listening on {p.ip}:{p.port} as {p.name} ({p.width}x{p.height})")

    if args.scale:
        scale = args.scale
    else:
        cols = shutil.get_terminal_size((120, 40)).columns
        total = sum(p.width for p in panels) + 2 * len(panels)
        scale = max(1, -(-total // max(cols - 4, 20)))

    started = time.monotonic()
    last_draw = 0.0
    print("\x1b[2J", end="")             # clear once; redraws are cursor-home
    try:
        while True:
            ready, _, _ = select.select(panels, [], [], 0.05)
            for p in ready:
                p.drain()

            now = time.monotonic()
            for p in panels:
                p.tick_rates(now)

            if now - last_draw < 0.1:
                continue
            last_draw = now

            buf = ["\x1b[H"]
            if not args.stats_only:
                blocks = [render(p, scale) for p in panels]
                height = max(len(b) for b in blocks)
                for row in range(height):
                    buf.append("  ".join(b[row] if row < len(b) else "" for b in blocks))
                    buf.append("\n")
            for p in panels:
                buf.append("\x1b[2K" + p.status() + "\n")
            buf.append(f"\x1b[2Kuptime {int(now - started)}s — Ctrl-C to stop\n")
            sys.stdout.write("".join(buf))
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n\nsummary")
        for p in panels:
            print(" ", p.status())


if __name__ == "__main__":
    main()
