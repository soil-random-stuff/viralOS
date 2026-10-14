#!/usr/bin/env python3
"""
Draw the mouse cursors: square RGBA PNGs (30x30 by default) in the style of
macOS, but rounder.

  arrow.png            the normal pointer (black, white outline)
  hand.png             links: a pointing hand (white, black outline)
  resize_ew.png        resize left/right
  resize_ns.png        resize up/down
  resize_nwse.png      resize along the top-left / bottom-right diagonal
  resize_nesw.png      resize along the top-right / bottom-left diagonal
  preview.png          all of them 4x larger, on a light and a dark background
  hotspots.txt         the pixel of each that does the pointing

Every shape is a signed distance function (negative inside, in pixels), so
corners round off naturally and edges are antialiased exactly. Each cursor
gets three layers: a soft shadow, an outline and the fill. The shapes are
designed on a 36x36 grid and drawn at whatever size is asked for. Only the
Python standard library is used.

Usage: scripts/make_cursors.py [output folder] [--size N] [--from-png NAME=FILE]...
  output folder       default: cursors/
  --size N            pixels square (default 30)
  --from-png NAME=FILE  instead of drawing cursor NAME, shrink this (hand-edited)
                      picture, drawn on the same 36x36 grid, to the size
"""

import math
import os
import struct
import sys
import zlib

SIZE = 36            # the design grid
DEFAULT_PIXELS = 30  # the cursors' size on screen

# --- distance functions --------------------------------------------------------------------


def sd_segment(px, py, ax, ay, bx, by):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = max(0.0, min(1.0, (pax * bax + pay * bay) / (bax * bax + bay * bay)))
    return math.hypot(pax - bax * h, pay - bay * h)


def sd_capsule(px, py, ax, ay, bx, by, r):
    return sd_segment(px, py, ax, ay, bx, by) - r


def sd_polygon(px, py, v):
    """Exact distance to a polygon (Inigo Quilez's method)."""
    d = (px - v[0][0]) ** 2 + (py - v[0][1]) ** 2
    s = 1.0
    j = len(v) - 1
    for i in range(len(v)):
        ex, ey = v[j][0] - v[i][0], v[j][1] - v[i][1]
        wx, wy = px - v[i][0], py - v[i][1]
        t = max(0.0, min(1.0, (wx * ex + wy * ey) / (ex * ex + ey * ey)))
        bx, by = wx - ex * t, wy - ey * t
        d = min(d, bx * bx + by * by)
        c1, c2, c3 = py >= v[i][1], py < v[j][1], ex * wy > ey * wx
        if (c1 and c2 and c3) or (not c1 and not c2 and not c3):
            s = -s
        j = i
    return s * math.sqrt(d)


def sd_round_box(px, py, cx, cy, hw, hh, r):
    qx, qy = abs(px - cx) - hw + r, abs(py - cy) - hh + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def smin(a, b, k):
    """A union that fills the inside corner where shapes meet with a curve."""
    h = max(k - abs(a - b), 0.0) / k
    return min(a, b) - h * h * k * 0.25


def inset(points, cx, cy, by):
    """Shrink a polygon towards a point, so rounding it (-r) keeps it about the same size."""
    out = []
    for x, y in points:
        dx, dy = x - cx, y - cy
        n = math.hypot(dx, dy) or 1.0
        out.append((x - dx / n * by, y - dy / n * by))
    return out


# --- the shapes -------------------------------------------------------------------------------
#
# Each returns (distance to the body, distance to detail lines or None) for a
# point in the 36x36 design space.

ARROW = [(0, 0), (0, 16.5), (4.1, 12.7), (6.9, 19.0), (9.5, 17.9), (6.8, 11.8), (11.8, 11.8)]
ARROW = [(8 + x * 1.32, 5 + y * 1.32) for x, y in ARROW]
ARROW_ROUND = 1.1
ARROW = inset(ARROW, 13, 18, ARROW_ROUND * 0.6)


def arrow(px, py):
    return sd_polygon(px, py, ARROW) - ARROW_ROUND, None


def hand(px, py):
    finger = sd_capsule(px, py, 14.6, 6.6, 14.6, 17.5, 2.75)
    middle = sd_capsule(px, py, 19.5, 14.4, 19.5, 19.5, 2.55)
    ring = sd_capsule(px, py, 24.0, 15.8, 24.0, 20.6, 2.4)
    pinky = sd_capsule(px, py, 28.0, 17.6, 28.0, 21.8, 2.2)
    palm = sd_round_box(px, py, 21.3, 24.6, 8.5, 6.3, 4.6)
    thumb = sd_capsule(px, py, 8.6, 19.8, 13.4, 25.6, 2.5)
    d = smin(finger, palm, 1.2)
    for part in (middle, ring, pinky):
        d = smin(d, part, 1.0)
    d = smin(d, thumb, 1.6)
    # The creases between the folded fingers, and where the thumb meets the palm.
    lines = min(
        sd_capsule(px, py, 17.05, 17.2, 17.05, 21.2, 0.45),
        sd_capsule(px, py, 21.75, 18.6, 21.75, 21.9, 0.45),
        sd_capsule(px, py, 26.0, 19.8, 26.0, 22.6, 0.45),
    )
    return d, lines


def double_arrow(angle, length=11.6, head=7.0, half=5.6):
    """A double-headed arrow through the middle, turned by `angle` degrees."""
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)
    right = inset([(length, 0), (length - head, -half), (length - head, half)], length - head / 2, 0, 0.75)
    left = [(-x, y) for x, y in right]

    def shape(px, py):
        # Into the arrow's own frame: u along it, v across.
        x, y = px - 18, py - 18
        u, v = x * ca + y * sa, -x * sa + y * ca
        heads = min(sd_polygon(u, v, right), sd_polygon(u, v, left)) - 0.9
        shaft = sd_capsule(u, v, -length + head - 1, 0, length - head + 1, 0, 1.35)
        return smin(heads, shaft, 1.2), None

    return shape


# --- drawing ------------------------------------------------------------------------------------

BLACK = (0.08, 0.08, 0.09)
WHITE = (1.0, 1.0, 1.0)


def smoothstep(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def over(dst, rgb, a):
    """Paint colour `rgb` with alpha `a` over dst (r, g, b, a), straight alpha."""
    dr, dg, db, da = dst
    out_a = a + da * (1 - a)
    if out_a <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    mix = lambda s, d: (s * a + d * da * (1 - a)) / out_a
    return (mix(rgb[0], dr), mix(rgb[1], dg), mix(rgb[2], db), out_a)


def render(shape, style, scale=1.0, background=None):
    """
    style: (fill colour, outline colour, outline width, line colour, rim width).
    The rim is a thin light edge outside the outline, so a dark outline still
    shows on dark backgrounds.
    Returns rows of (r, g, b, a) floats, SIZE*scale square (the 36-unit design
    grid drawn at `scale` pixels per unit).
    """
    fill, outline, width, line_color, rim = style
    n = round(SIZE * scale)
    rows = []
    for j in range(n):
        row = []
        for i in range(n):
            px, py = (i + 0.5) / scale, (j + 0.5) / scale
            body, lines = shape(px, py)
            shadow_d, _ = shape(px - 0.0, py - 1.1)   # light from above: the shadow sits lower
            aa = 0.5 / scale + 0.001

            pixel = background if background else (0.0, 0.0, 0.0, 0.0)
            # Soft shadow around the outline.
            pixel = over(pixel, (0, 0, 0), 0.32 * smoothstep(2.4, -1.0, shadow_d - width - rim))
            if rim:
                pixel = over(pixel, (0.96, 0.96, 0.97), 0.9 * smoothstep(aa, -aa, body - width - rim))
            # Outline, then the fill inside it.
            pixel = over(pixel, outline, smoothstep(aa, -aa, body - width))
            pixel = over(pixel, fill, smoothstep(aa, -aa, body))
            if lines is not None:
                inside = smoothstep(aa, -aa, body + 0.6)
                pixel = over(pixel, line_color, inside * smoothstep(aa, -aa, lines))
            row.append(pixel)
        rows.append(row)
    return rows


def write_png(path, rows):
    height, width = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)   # filter: none
        for r, g, b, a in row:
            raw += bytes(max(0, min(255, round(c * 255))) for c in (r, g, b, a))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def read_png(path):
    """8-bit RGB or RGBA PNG -> rows of (r, g, b, a) floats."""
    data = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color not in (2, 6) or body[12] != 0:
                raise SystemExit(f"{path}: only 8-bit RGB/RGBA, not interlaced")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    bpp = 4 if color == 6 else 3
    raw, stride = zlib.decompress(idat), width * bpp
    rows, prev, o = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        rows.append([tuple(line[i + k] / 255 for k in range(3)) + ((line[i + 3] / 255,) if bpp == 4 else (1.0,))
                     for i in range(0, stride, bpp)])
    return rows


def shrink(rows, size):
    """Area-average a square picture down to size x size (in premultiplied alpha, so edges stay clean)."""
    n = len(rows)
    k = n / size
    out = []
    for j in range(size):
        row = []
        for i in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            y0, y1, x0, x1 = j * k, (j + 1) * k, i * k, (i + 1) * k
            for y in range(int(y0), min(n, math.ceil(y1))):
                wy = min(y + 1, y1) - max(y, y0)
                for x in range(int(x0), min(n, math.ceil(x1))):
                    w = wy * (min(x + 1, x1) - max(x, x0))
                    r, g, b, a = rows[y][x]
                    acc[0] += r * a * w
                    acc[1] += g * a * w
                    acc[2] += b * a * w
                    acc[3] += a * w
                    total += w
            a = acc[3] / total
            row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a) if acc[3] > 0 else (0, 0, 0, 0))
        out.append(row)
    return out


# --- the set ---------------------------------------------------------------------------------------

DARK = (BLACK, WHITE, 1.7, None, 0)       # black body, white outline
LIGHT = (WHITE, BLACK, 1.3, BLACK, 0.8)   # white body, black outline, light rim (the hand)

CURSORS = [
    # name, shape, style, hot spot (the pixel that points, on the 36x36 design grid)
    ("arrow", arrow, DARK, (7, 4)),
    ("hand", hand, LIGHT, (14, 3)),
    ("resize_ew", double_arrow(0), DARK, (18, 18)),
    ("resize_ns", double_arrow(90), DARK, (18, 18)),
    ("resize_nwse", double_arrow(45, 12.4, 6.3, 5.0), DARK, (18, 18)),
    ("resize_nesw", double_arrow(-45, 12.4, 6.3, 5.0), DARK, (18, 18)),
]


def main(argv):
    out, pixels, from_png = "cursors", DEFAULT_PIXELS, {}
    args = argv[1:]
    while args:
        arg = args.pop(0)
        if arg == "--size":
            pixels = int(args.pop(0))
        elif arg == "--from-png":
            name, _, path = args.pop(0).partition("=")
            from_png[name] = path
        else:
            out = arg
    scale = pixels / SIZE
    os.makedirs(out, exist_ok=True)
    pictures = {}
    for name, shape, style, _ in CURSORS:
        if name in from_png:
            pictures[name] = shrink(read_png(from_png[name]), pixels)
        else:
            pictures[name] = render(shape, style, scale)
        write_png(os.path.join(out, name + ".png"), pictures[name])

    # Preview: 4x (blown up pixel by pixel, to show them as they'll look), on light and dark.
    zoom, pad = 4, 12
    cell = pixels * zoom + 2 * pad
    backgrounds = [(0.93, 0.94, 0.96, 1.0), (0.17, 0.18, 0.23, 1.0)]
    sheet = [[backgrounds[j // cell] for _ in range(cell * len(CURSORS))] for j in range(cell * 2)]
    for k, (name, _, _, _) in enumerate(CURSORS):
        for b, bg in enumerate(backgrounds):
            for j in range(pixels * zoom):
                for i in range(pixels * zoom):
                    r, g, bl, a = pictures[name][j // zoom][i // zoom]
                    sheet[b * cell + pad + j][k * cell + pad + i] = over(bg, (r, g, bl), a)
    write_png(os.path.join(out, "preview.png"), sheet)

    with open(os.path.join(out, "hotspots.txt"), "w", newline="\n") as f:
        f.write("# Cursor hot spots: the pixel (x, y) that does the pointing.\n")
        for name, _, _, (x, y) in CURSORS:
            f.write(f"{name}.png {int((x + 0.5) * scale)} {int((y + 0.5) * scale)}\n")
    print(f"wrote {len(CURSORS)} {pixels}x{pixels} cursors and preview.png to {out}/")


if __name__ == "__main__":
    main(sys.argv)
