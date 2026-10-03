#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the deterministic nesting responsiveness SVG fixtures.

Standard library only.  Usage:

    python3 testfiles/nesting/responsiveness/make_fixtures.py [output_dir]

CTest runs it as the fixture setup of test_nesting-responsiveness, writing
into the build tree (the fixtures are about 22 MB and are not committed).  The
output directory defaults to ``testfiles/nesting/responsiveness`` and is
created if missing.  Four ``<fixture>.svg`` files and four matching
``<fixture>.ids.txt`` files are written with LF line endings.  Output is
deterministic on one host; the embedded PNG bytes depend on the zlib build, so
hashes can differ between hosts while the decoded pixels do not.
"""

import base64
import math
import pathlib
import struct
import sys
import zlib

OUT = pathlib.Path(
    sys.argv[1] if len(sys.argv) > 1 else "testfiles/nesting/responsiveness"
)
OUT.mkdir(parents=True, exist_ok=True)

HEADER = (
    '<svg xmlns="http://www.w3.org/2000/svg" '
    'xmlns:xlink="http://www.w3.org/1999/xlink" '
    'width="6000" height="3200" viewBox="0 0 6000 3200">\n'
    '<rect id="sheet" x="0" y="0" width="3000" height="3000" fill="none" '
    'stroke="#888" stroke-width="0.3"/>\n'
)
FOOTER = "</svg>\n"


def cell_origin(i, cols=10, cell_w=290, cell_h=300):
    """Grid cell origin started at x=3100, y=40, laid out off the sheet."""
    col = i % cols
    row = i // cols
    return 3100 + cell_w * col, 40 + cell_h * row


def part_id(i):
    return "part-%03d" % (i + 1)


def write_fixture(name, parts, ids):
    body = "\n".join(parts) + "\n"
    (OUT / (name + ".svg")).write_bytes((HEADER + body + FOOTER).encode("ascii"))
    (OUT / (name + ".ids.txt")).write_bytes(("\n".join(ids) + "\n").encode("ascii"))


def fixture_vector_cards_200():
    """200 rounded cards, each with 30 closed 24-gon vector paths inside."""
    parts, ids = [], []
    for i in range(200):
        x, y = cell_origin(i, cols=10, cell_w=350, cell_h=210)
        pid = part_id(i)
        ids.append(pid)
        radius = 12 + (i % 5)
        inner = []
        for j in range(30):
            ccx = 24 + 48 * (j % 6)
            ccy = 24 + 36 * (j // 6)
            points = []
            for k in range(24):
                a = 2.0 * math.pi * k / 24
                px = ccx + radius * math.cos(a)
                py = ccy + radius * math.sin(a)
                points.append(f"{px:.3f},{py:.3f}")
            inner.append('<path d="M ' + " L ".join(points) + ' Z" fill="#000"/>')
        parts.append(
            f'<g id="{pid}" transform="translate({x:.3f},{y:.3f})">'
            '<rect x="0" y="0" width="336" height="192" rx="12" ry="12" '
            'fill="none" stroke="#000" stroke-width="1"/>'
            + "".join(inner)
            + "</g>"
        )
    write_fixture("vector_cards_200", parts, ids)


def fixture_detailed_curves_240():
    """240 unique closed 1500-vertex curves (12 columns)."""
    parts, ids = [], []
    vertices = 1500
    for i in range(240):
        ox, oy = cell_origin(i, cols=12, cell_w=290, cell_h=300)
        cx, cy = ox + 100, oy + 100
        radius = 40.0 * (1.0 + 0.002 * i)
        points = []
        for k in range(vertices):
            a = 2.0 * math.pi * k / vertices
            r = radius * (
                1.0
                + 0.15 * math.sin(5 * a)
                + 0.04 * math.cos(23 * a)
                + 0.01 * math.sin(71 * a)
            )
            px = cx + r * math.cos(a)
            py = cy + r * math.sin(a)
            points.append(f"{px:.3f},{py:.3f}")
        pid = part_id(i)
        ids.append(pid)
        parts.append(f'<path id="{pid}" d="M ' + " L ".join(points) + ' Z" fill="#000"/>')
    write_fixture("detailed_curves_240", parts, ids)


def _png_chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def make_disc_png(radius):
    """A 2000x2000 RGBA PNG: red disc radius R, linear alpha ramp over 20px."""
    size = 2000
    centre = 1000
    r_in2 = radius * radius
    r_out2 = (radius + 20) * (radius + 20)
    row_template = bytes((200, 30, 30, 0)) * size
    rows = []
    for y in range(size):
        dy = y - centre
        dy2 = dy * dy
        alpha = bytearray(size)
        lo = r_in2 - dy2
        inside = math.isqrt(lo) if lo >= 0 else -1
        if inside >= 0:
            a = centre - inside
            b = centre + inside + 1
            if a < 0:
                a = 0
            if b > size:
                b = size
            alpha[a:b] = b"\xff" * (b - a)
        hi = r_out2 - dy2
        if hi >= 0:
            outside = math.isqrt(hi)
            for dx in range(inside + 1, outside + 1):
                d = math.sqrt(dx * dx + dy2)
                v = int(round(255 * (1.0 - (d - radius) / 20.0)))
                if v < 0:
                    v = 0
                elif v > 255:
                    v = 255
                xr = centre + dx
                if xr < size:
                    alpha[xr] = v
                xl = centre - dx
                if xl >= 0:
                    alpha[xl] = v
        row = bytearray(row_template)
        row[3::4] = alpha
        rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(raw, 9))
        + _png_chunk(b"IEND", b"")
    )


def fixture_stickers_bitmap_50():
    """50 embedded PNG rasters, each with its own 2000x2000 RGBA payload."""
    parts, ids = [], []
    for i in range(50):
        x, y = cell_origin(i, cols=10, cell_w=290, cell_h=300)
        pid = part_id(i)
        ids.append(pid)
        png = make_disc_png(700 + 4 * i)
        uri = "data:image/png;base64," + base64.b64encode(png).decode("ascii")
        parts.append(
            f'<image id="{pid}" x="{x:.3f}" y="{y:.3f}" width="200" height="200" '
            f'xlink:href="{uri}"/>'
        )
    write_fixture("stickers_bitmap_50", parts, ids)


def fixture_stroked_paths_100():
    """100 open 600-vertex stroked paths (no fill, round join/cap)."""
    parts, ids = [], []
    vertices = 600
    for i in range(100):
        ox, oy = cell_origin(i, cols=10, cell_w=290, cell_h=300)
        cx, cy = ox + 120, oy + 120
        freq = 3 + (i % 4)
        points = []
        for k in range(vertices):
            t = k / 599
            px = cx + 100.0 * t - 50.0
            py = (
                cy
                + 40.0 * math.sin(2 * math.pi * t * freq)
                + 10.0 * math.sin(2 * math.pi * t * 17)
            )
            points.append(f"{px:.3f},{py:.3f}")
        pid = part_id(i)
        ids.append(pid)
        parts.append(
            f'<path id="{pid}" d="M '
            + " L ".join(points)
            + '" fill="none" stroke="#000" stroke-width="6" '
            'stroke-linejoin="round" stroke-linecap="round"/>'
        )
    write_fixture("stroked_paths_100", parts, ids)


def main():
    fixture_vector_cards_200()
    fixture_detailed_curves_240()
    fixture_stickers_bitmap_50()
    fixture_stroked_paths_100()


if __name__ == "__main__":
    main()
