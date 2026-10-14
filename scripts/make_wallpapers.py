#!/usr/bin/env python3
"""
Draw the stock wallpapers (flat-colour mountain landscapes) as PNG files.

    make_wallpapers.py OUTPUT_DIR

Writes default.png (dusk) and night.png at 1920x1080. They're plain images:
replace or add any .png in resources/Backgrounds and it shows up in
the wallpaper menu. Only the Python standard library is used; as a bonus
for testing the OS's decoder, rows cycle through all five PNG filter
types and the data is split over several IDAT chunks.
"""

import math
import os
import random
import struct
import sys
import zlib

W, H = 1920, 1080


def hex_rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


SCENES = {
    "default": {   # dusk
        "sky": "#F6D3A9",
        "sun": ("#FFF0D2", 1340, 405, 118),
        "moon": None,
        "stars": 0,
        "layers": ["#EBAA8C", "#CD8286", "#93607F", "#5D426D", "#2F2B50"],
    },
    "night": {
        "sky": "#18203B",
        "sun": None,
        "moon": ("#F1EAD2", 520, 260, 70),
        "stars": 260,
        "layers": ["#2A3664", "#222D57", "#1B2547", "#141C38", "#0D1326"],
    },
}


def ridges(seed):
    """Height of each mountain layer's ridge, per column (far layers first)."""
    rnd = random.Random(seed)
    out = []
    for i in range(5):
        base = H * (0.52 + 0.095 * i)
        waves = [(rnd.uniform(0.0006, 0.0016) * (1 + i * 0.3), rnd.uniform(0, 6.3), H * rnd.uniform(0.035, 0.07)),
                 (rnd.uniform(0.003, 0.006), rnd.uniform(0, 6.3), H * rnd.uniform(0.015, 0.03)),
                 (rnd.uniform(0.009, 0.016), rnd.uniform(0, 6.3), H * rnd.uniform(0.004, 0.01))]
        peak_f, peak_p, peak_a = rnd.uniform(0.0025, 0.004), rnd.uniform(0, 1), H * 0.03 * (1 - i * 0.12)
        row = []
        for x in range(W):
            y = base
            for f, p, a in waves:
                y += a * math.sin(x * f + p)
            t = (x * peak_f + peak_p) % 1.0          # triangle wave: pointy peaks
            y -= peak_a * (1 - abs(2 * t - 1))
            row.append(y)
        out.append(row)
    return out


def disc(px, py, cx, cy, r):
    """Coverage of a pixel by a circle (smooth edge)."""
    d = math.hypot(px + 0.5 - cx, py + 0.5 - cy)
    return max(0.0, min(1.0, r - d + 0.5))


def mix(a, b, t):
    return tuple(int(a[k] + (b[k] - a[k]) * t + 0.5) for k in range(3))


def render(scene, seed):
    sky = hex_rgb(scene["sky"])
    layers = [hex_rgb(c) for c in scene["layers"]]
    tops = ridges(seed)
    rnd = random.Random(seed + 1)
    stars = {}
    for _ in range(scene["stars"]):
        x, y = rnd.randrange(W), rnd.randrange(int(H * 0.6))
        stars[(x, y)] = rnd.uniform(0.35, 1.0)

    rows = []
    for y in range(H):
        row = bytearray(W * 3)
        for x in range(W):
            c = sky
            if (x, y) in stars:
                c = mix(c, (232, 236, 248), stars[(x, y)])
            for body in (scene["sun"], scene["moon"]):
                if body and abs(y - body[2]) <= body[3] + 1:
                    cover = disc(x, y, body[1], body[2], body[3])
                    if scene["moon"] is body:   # a crescent: a second disc takes a bite
                        cover = max(0.0, cover - disc(x, y, body[1] + body[3] * 0.45, body[2] - body[3] * 0.25, body[3]))
                    if cover:
                        c = mix(c, hex_rgb(body[0]), cover)
            for i, top in enumerate(tops):     # far to near: nearer layers paint over
                t = tops[i][x]
                if y + 1 > t:
                    c = mix(c, layers[i], min(1.0, y + 1 - t))
            row[3 * x:3 * x + 3] = bytes(c)
        rows.append(row)
    return rows


def filtered(rows, bpp=3):
    """Apply PNG filters, cycling through all five types."""
    out = bytearray()
    prev = bytearray(len(rows[0]))
    for y, row in enumerate(rows):
        ftype = y % 5
        out.append(ftype)
        for i, v in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 0:
                pred = 0
            elif ftype == 1:
                pred = a
            elif ftype == 2:
                pred = b
            elif ftype == 3:
                pred = (a + b) // 2
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out.append((v - pred) & 0xFF)
        prev = row
    return bytes(out)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def write_png(path, rows):
    data = zlib.compress(filtered(rows), 9)
    parts = [data[i:i + 65536] for i in range(0, len(data), 65536)]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
        for part in parts:
            f.write(chunk(b"IDAT", part))
        f.write(chunk(b"IEND", b""))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)
    for i, (name, scene) in enumerate(SCENES.items()):
        path = os.path.join(out_dir, name + ".png")
        write_png(path, render(scene, seed=7 + i))
        print(f"{path}: {os.path.getsize(path)} bytes")


if __name__ == "__main__":
    main()
