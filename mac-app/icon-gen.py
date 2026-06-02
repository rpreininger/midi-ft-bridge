#!/usr/bin/env python3
"""Generate the MIDI-FT Bridge app icon: a 5-pin DIN MIDI plug above a
suspension bridge, on a gradient squircle. Rendered at 2x then downscaled."""
import math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

W = 2048
OUT = "/tmp/icon_1024.png"

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def vgrad(size, top, bottom):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        g.putpixel((0, y), lerp(top, bottom, y / (size - 1)))
    return g.resize((size, size))

# --- squircle geometry ---
m = int(0.085 * W)
rect = [m, m, W - m, W - m]
radius = int(0.235 * W)

mask = Image.new("L", (W, W), 0)
ImageDraw.Draw(mask).rounded_rectangle(rect, radius=radius, fill=255)

# --- drop shadow ---
canvas = Image.new("RGBA", (W, W), (0, 0, 0, 0))
sh_a = Image.new("L", (W, W), 0)
ImageDraw.Draw(sh_a).rounded_rectangle(
    [rect[0] + 6, rect[1] + 26, rect[2] + 6, rect[3] + 40], radius=radius, fill=150)
sh_a = sh_a.filter(ImageFilter.GaussianBlur(36))
shadow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
shadow.putalpha(sh_a)
canvas = Image.alpha_composite(canvas, shadow)

# --- gradient background ---
bg = Image.new("RGBA", (W, W), (0, 0, 0, 0))
grad = vgrad(W, (58, 46, 120), (19, 176, 165)).convert("RGBA")
bg.paste(grad, (0, 0), mask)
# top sheen
sheen = Image.new("L", (W, W), 0)
ImageDraw.Draw(sheen).ellipse([m - 200, m - 600, W - m + 200, int(0.5 * W)], fill=46)
sheen = ImageChops.multiply(sheen, mask)
sheen_img = Image.new("RGBA", (W, W), (255, 255, 255, 0))
sheen_img.putalpha(sheen)
bg = Image.alpha_composite(bg, sheen_img)
canvas = Image.alpha_composite(canvas, bg)

# --- content layer (bridge + plug), clipped to squircle ---
content = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(content)
def P(nx, ny):
    return (nx * W, ny * W)

CREAM = (255, 246, 232)
CREAM_DIM = (255, 246, 232, 210)

# Bridge layout (normalized)
deck_y = 0.715
tower_top = 0.485
lx, rx = 0.345, 0.655           # tower x
ax_l, ax_r = 0.175, 0.825       # cable anchors
anchor_y = 0.625
lw = int(0.012 * W)             # cable line width
tower_w = int(0.020 * W)

# main cable: anchor -> tower top -> sag -> tower top -> anchor
def catenary(x0, x1, ytop, ysag, n=60):
    pts = []
    for i in range(n + 1):
        t = i / n
        x = x0 + (x1 - x0) * t
        # parabola, lowest (largest y) at midpoint
        y = ytop + (ysag - ytop) * (1 - (2 * t - 1) ** 2)
        pts.append(P(x, y))
    return pts

cable = [P(ax_l, anchor_y), P(lx, tower_top)]
cable += catenary(lx, rx, tower_top, 0.560)
cable += [P(rx, tower_top), P(ax_r, anchor_y)]
d.line(cable, fill=CREAM, width=lw, joint="curve")

# suspenders (vertical hangers from cable to deck)
def cable_y_at(x):
    if x <= lx:
        t = (x - ax_l) / (lx - ax_l)
        return anchor_y + (tower_top - anchor_y) * t
    if x >= rx:
        t = (x - rx) / (ax_r - rx)
        return tower_top + (anchor_y - tower_top) * t
    t = (x - lx) / (rx - lx)
    return tower_top + (0.560 - tower_top) * (1 - (2 * t - 1) ** 2)

xx = lx
while xx <= rx + 1e-6:
    cy = cable_y_at(xx)
    if cy < deck_y - 0.004:
        d.line([P(xx, cy), P(xx, deck_y)], fill=CREAM_DIM, width=int(0.0045 * W))
    xx += (rx - lx) / 9

# deck
d.line([P(0.135, deck_y), P(0.865, deck_y)], fill=CREAM, width=int(0.016 * W))
d.line([P(0.135, deck_y + 0.028), P(0.865, deck_y + 0.028)], fill=CREAM_DIM,
       width=int(0.006 * W))

# towers
for tx in (lx, rx):
    d.rounded_rectangle([tx * W - tower_w, tower_top * W,
                         tx * W + tower_w, (deck_y + 0.012) * W],
                        radius=tower_w, fill=CREAM)

# --- MIDI 5-pin DIN plug (hero) ---
cx, cy = 0.5 * W, 0.355 * W
R = 0.165 * W

# soft glow
glow = Image.new("L", (W, W), 0)
ImageDraw.Draw(glow).ellipse([cx - R * 1.7, cy - R * 1.7, cx + R * 1.7, cy + R * 1.7],
                             fill=120)
glow = glow.filter(ImageFilter.GaussianBlur(60))
glow = ImageChops.multiply(glow, mask)
glow_img = Image.new("RGBA", (W, W), (255, 255, 255, 0))
glow_img.putalpha(glow)
content = Image.alpha_composite(content, glow_img)
d = ImageDraw.Draw(content)

# metallic outer shell with gradient ring
shell = vgrad(int(R * 2), (245, 248, 252), (176, 186, 200)).convert("RGBA")
shell_mask = Image.new("L", (int(R * 2), int(R * 2)), 0)
ImageDraw.Draw(shell_mask).ellipse([0, 0, int(R * 2) - 1, int(R * 2) - 1], fill=255)
content.paste(shell, (int(cx - R), int(cy - R)), shell_mask)
d = ImageDraw.Draw(content)
d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=(120, 130, 148), width=int(0.006 * W))

# notch / keyway tab at the bottom
nw = R * 0.34
d.rounded_rectangle([cx - nw, cy + R - R * 0.06, cx + nw, cy + R + R * 0.16],
                    radius=int(R * 0.10), fill=(214, 221, 230),
                    outline=(120, 130, 148), width=int(0.004 * W))

# dark connector face
rf = R * 0.80
d.ellipse([cx - rf, cy - rf, cx + rf, cy + rf], fill=(26, 31, 46))
d.ellipse([cx - rf, cy - rf, cx + rf, cy + rf], outline=(64, 72, 92),
          width=int(0.004 * W))

# 5 pins on the upper semicircle arc
pin_r = R * 0.115
pin_ring = R * 0.50
for ang in (0, 45, 90, 135, 180):
    a = math.radians(ang)
    px = cx + pin_ring * math.cos(a)
    py = cy - pin_ring * math.sin(a)   # sin>=0 -> upper half
    d.ellipse([px - pin_r, py - pin_r, px + pin_r, py + pin_r],
              fill=(240, 202, 102), outline=(150, 110, 40), width=int(0.0028 * W))

# clip content to squircle
content.putalpha(ImageChops.multiply(content.split()[3], mask))
canvas = Image.alpha_composite(canvas, content)

# subtle outline on the squircle edge
edge = Image.new("RGBA", (W, W), (0, 0, 0, 0))
ImageDraw.Draw(edge).rounded_rectangle(rect, radius=radius, outline=(255, 255, 255, 40),
                                       width=int(0.004 * W))
canvas = Image.alpha_composite(canvas, edge)

canvas = canvas.resize((1024, 1024), Image.LANCZOS)
canvas.save(OUT)
print("wrote", OUT)
