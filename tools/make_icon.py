#!/usr/bin/env python3
"""Writes artwork/PowerMac.hvif, the PowerMac emulator's vector icon.

HVIF is Haiku's compact vector icon format (what Icon-O-Matic exports). The
encoding follows src/libs/icon/flat_icon/FlatIconImporter.cpp in the Haiku
source tree: a magic number, then styles, paths and shapes. Coordinates are
in a 64 x 64 space. (The helpers come from BeNC's tools/make_icon.py.)

The icon: a platinum Power Macintosh tower in Haiku's usual three-quarter
view, with the CD-ROM drive and the floppy slot at the top of the front,
the vents below and the green power light.
"""

import math
import os
import struct
import sys

STYLE_SOLID_COLOR = 1
STYLE_GRADIENT = 2
STYLE_SOLID_COLOR_NO_ALPHA = 3
SHAPE_PATH_SOURCE = 10

GRADIENT_LINEAR = 0
GRADIENT_CIRCULAR = 1
GRADIENT_FLAG_TRANSFORM = 1 << 1
GRADIENT_FLAG_NO_ALPHA = 1 << 2

PATH_FLAG_CLOSED = 1 << 1
PATH_FLAG_NO_CURVES = 1 << 3


def coord(value):
    """One byte for small integers, otherwise 15 bit fixed point."""
    if float(value).is_integer() and -32 <= value <= 95:
        return bytes([int(value) + 32])
    raw = int(round((value + 128.0) * 102.0)) | 0x8000
    return bytes([raw >> 8, raw & 0xFF])


def float24(value):
    """1 sign bit, 6 bit exponent, 17 bit mantissa."""
    if value == 0.0:
        return b"\0\0\0"
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    sign = bits >> 31
    exponent = ((bits >> 23) & 0xFF) - 127
    mantissa = bits & 0x7FFFFF
    if exponent >= 32 or exponent < -32:
        return b"\0\0\0"
    short = (sign << 23) | ((exponent + 32) << 17) | (mantissa >> 6)
    return bytes([(short >> 16) & 0xFF, (short >> 8) & 0xFF, short & 0xFF])


def matrix(m):
    return b"".join(float24(v) for v in m)


def rgb(hex_color):
    hex_color = hex_color.lstrip("#")
    return bytes(int(hex_color[i:i + 2], 16) for i in (0, 2, 4))


# --- styles -----------------------------------------------------------------

def solid(hex_color, alpha=None):
    if alpha is None:
        return bytes([STYLE_SOLID_COLOR_NO_ALPHA]) + rgb(hex_color)
    return bytes([STYLE_SOLID_COLOR]) + rgb(hex_color) + bytes([alpha])


def linear_gradient(x0, y0, x1, y1, stops):
    """Gradient along the line (x0, y0) -> (x1, y1).

    stops: (offset 0..255, "#rrggbb", alpha or None). A linear gradient
    runs from -64 to 64 along x in gradient space; the matrix maps that
    onto the line.
    """
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy)
    s = length / 128.0
    c, n = dx / length, dy / length
    m = (c * s, n * s, -n * s, c * s, (x0 + x1) / 2.0, (y0 + y1) / 2.0)
    opaque = all(alpha is None for _, _, alpha in stops)
    flags = GRADIENT_FLAG_TRANSFORM | (GRADIENT_FLAG_NO_ALPHA if opaque else 0)
    data = bytes([STYLE_GRADIENT, GRADIENT_LINEAR, flags, len(stops)])
    data += matrix(m)
    for offset, color, alpha in stops:
        data += bytes([offset]) + rgb(color)
        if not opaque:
            data += bytes([255 if alpha is None else alpha])
    return data


def radial_gradient(cx, cy, radius, stops):
    s = radius / 64.0
    m = (s, 0.0, 0.0, s, cx, cy)
    data = bytes([STYLE_GRADIENT, GRADIENT_CIRCULAR,
                  GRADIENT_FLAG_TRANSFORM | GRADIENT_FLAG_NO_ALPHA,
                  len(stops)])
    data += matrix(m)
    for offset, color in stops:
        data += bytes([offset]) + rgb(color)
    return data


# --- paths ------------------------------------------------------------------

def polygon(points):
    data = bytes([PATH_FLAG_CLOSED | PATH_FLAG_NO_CURVES, len(points)])
    for x, y in points:
        data += coord(x) + coord(y)
    return data


def curves(points):
    """Closed path of (point, in-handle, out-handle) triples."""
    data = bytes([PATH_FLAG_CLOSED, len(points)])
    for point, handle_in, handle_out in points:
        for x, y in (point, handle_in, handle_out):
            data += coord(x) + coord(y)
    return data


def ellipse(cx, cy, rx, ry, degrees=0.0):
    """An ellipse rotated around its center, as four Bezier segments."""
    k = 0.5523
    a = math.radians(degrees)
    ca, sa = math.cos(a), math.sin(a)

    def at(x, y):
        return (cx + x * ca - y * sa, cy + x * sa + y * ca)

    return curves([
        (at(0, -ry), at(-k * rx, -ry), at(k * rx, -ry)),
        (at(rx, 0), at(rx, -k * ry), at(rx, k * ry)),
        (at(0, ry), at(k * rx, ry), at(-k * rx, ry)),
        (at(-rx, 0), at(-rx, k * ry), at(-rx, -k * ry)),
    ])


def circle(cx, cy, r):
    return ellipse(cx, cy, r, r)


def quad(points):
    return polygon(points)


# --- shapes -----------------------------------------------------------------

def shape(style, paths):
    return bytes([SHAPE_PATH_SOURCE, style, len(paths)]) + bytes(paths) \
        + bytes([0])



def rounded_rect(x0, y0, x1, y1, r):
    """A rectangle with chamfered corners (close enough at icon sizes)."""
    return polygon([(x0 + r, y0), (x1 - r, y0), (x1, y0 + r), (x1, y1 - r),
                    (x1 - r, y1), (x0 + r, y1), (x0, y1 - r), (x0, y0 + r)])


def ring_segment(cx, cy, inner, outer, start, end, steps=10):
    """A piece of a ring between two angles (degrees, 0 = right, counter
    clockwise, y down)."""
    points = []
    for i in range(steps + 1):
        a = math.radians(start + (end - start) * i / steps)
        points.append((cx + outer * math.cos(a), cy - outer * math.sin(a)))
    for i in range(steps, -1, -1):
        a = math.radians(start + (end - start) * i / steps)
        points.append((cx + inner * math.cos(a), cy - inner * math.sin(a)))
    return polygon(points)



def build():
    styles = [
        solid("#3B3830"),                                           # 0 outline
        linear_gradient(16, 10, 44, 58, [(0, "#F2EFE7", None),
                                         (255, "#C7C0B0", None)]),   # 1 front
        linear_gradient(44, 8, 54, 56, [(0, "#B8B1A1", None),
                                        (255, "#857E70", None)]),    # 2 side
        linear_gradient(20, 3, 52, 9, [(0, "#FBF9F4", None),
                                       (255, "#DAD4C6", None)]),     # 3 top
        solid("#000000", 70),                                       # 4 shadow
        solid("#2A2823"),                                           # 5 slot
        linear_gradient(18, 12, 40, 21, [(0, "#DCD6C8", None),
                                         (255, "#B3AC9C", None)]),   # 6 bay
        solid("#9C9585"),                                           # 7 vent
        radial_gradient(37.5, 53.5, 2.6, [(0, "#B8FF8A"),
                                          (255, "#2FA51A")]),        # 8 led
        solid("#FFFFFF", 150),                                      # 9 edge
    ]

    paths = [
        ellipse(34, 60.5, 22, 2.6),                                 # 0
        polygon([(14, 9), (44, 9), (44, 59), (14, 59)]),            # 1 front o
        polygon([(15, 10), (43, 10), (43, 58), (15, 58)]),          # 2 front
        polygon([(44, 9), (53, 4), (53, 54), (44, 59)]),            # 3 side o
        polygon([(44.8, 9.6), (52.2, 5.4), (52.2, 53.4), (44.8, 57.6)]),  # 4
        polygon([(14, 9), (23, 4), (53, 4), (44, 9)]),              # 5 top o
        polygon([(16, 8.6), (23.4, 4.8), (50.6, 4.8), (43.4, 8.6)]),  # 6 top
        polygon([(15, 10), (43, 10), (43, 11), (15, 11)]),          # 7 edge
        rounded_rect(18, 13, 40, 21, 1),                            # 8 CD bay
        polygon([(20, 16.5), (38, 16.5), (38, 18), (20, 18)]),      # 9 tray
        polygon([(24, 25), (37, 25), (37, 26.6), (24, 26.6)]),      # 10 floppy
        circle(37.5, 53.5, 1.6),                                    # 11 led
        rounded_rect(19, 52, 27, 55, 1),                            # 12 power
    ]
    vents = []
    for i in range(6):
        y = 32 + i * 3
        vents.append(len(paths))
        paths.append(polygon([(19, y), (39, y), (39, y + 1.2), (19, y + 1.2)]))

    shapes = [
        shape(4, [0]),
        shape(0, [1]),
        shape(1, [2]),
        shape(0, [3]),
        shape(2, [4]),
        shape(0, [5]),
        shape(3, [6]),
        shape(9, [7]),
        shape(0, [8]),
        shape(6, [8]),
        shape(5, [9, 10]),
        shape(7, vents + [12]),
        shape(8, [11]),
    ]

    data = b"ncif"
    data += bytes([len(styles)]) + b"".join(styles)
    data += bytes([len(paths)]) + b"".join(paths)
    data += bytes([len(shapes)]) + b"".join(shapes)
    return data


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "artwork", "PowerMac.hvif")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
