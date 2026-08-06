#!/usr/bin/env python3
"""Generate src/components/icons/libraryIcons.h — the Library's own icons.

Two things set this apart from the general icon pipeline (freeink-sdk
gen_icons.py):

1. The favorites star is rasterised from an exact five-point polygon by pixel
   coverage, not thresholded from a stroked SVG: the threshold binarises
   antialiased edges one pixel at a time, and a mathematically symmetric star
   came out lopsided at 16 px. Coverage sampling on a symmetric grid is
   mirror-symmetric by construction (asserted below).

2. Every bitmap is stored pre-rotated 90 degrees COUNTER-clockwise. The
   raw-bits GfxRenderer::drawIcon path displays stored bitmaps rotated 90
   degrees clockwise on the portrait X3 — settled empirically over three
   flashes: upright-stored art leaned, clockwise-stored art stood on its head.
   Every icon in the app rides through the same blit; near-rotation-invariant
   shapes (cog, search, wifi) hid it for the whole icon set, and the row book
   icon has been lying on its side since the day it shipped. The star made it
   visible, and the book generated here stands the shelf's rows upright too.
   fillRect-drawn marks (the Titles triangle) never had the problem.

Requires rsvg-convert (librsvg) and Pillow for the book, like gen_icons.py.

Usage: python3 scripts/gen_star_icon.py   (writes the header in place)
"""

import io
import math
import subprocess

from PIL import Image

OUT = 'src/components/icons/libraryIcons.h'
BOOK_SVG = 'freeink-sdk/libs/assets/Icons/lucide/icons/book.svg'
STAR_SIZES = (16, 24)
SAMPLES = 8          # supersampling grid per pixel axis
COVERAGE = 0.38      # ink threshold; below 0.5 so 1-px star tips survive
OUTER = 0.46         # outer radius, as a fraction of the box
INNER = 0.45         # inner radius, as a fraction of the outer (chunkier
                     # than the 0.382 pentagram ratio, for 1-bit legibility)
THRESHOLD = 110      # luminance cut for the stroked book, as gen_icons.py


def star_polygon(px):
    R = px * OUTER
    r = R * INNER
    cx = px / 2.0
    # A star has more mass above its midline than below; offsetting the centre
    # down centres the BOUNDING BOX, which is what the eye centres.
    cy = px / 2.0 + 0.0955 * R
    pts = []
    for i in range(5):
        ao = math.radians(90 + i * 72)
        ai = math.radians(126 + i * 72)
        pts.append((cx + R * math.cos(ao), cy - R * math.sin(ao)))
        pts.append((cx + r * math.cos(ai), cy - r * math.sin(ai)))
    return pts


def point_in_polygon(x, y, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def rasterize_star(px):
    poly = star_polygon(px)
    rows = []
    for y in range(px):
        row = []
        for x in range(px):
            hits = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    if point_in_polygon(x + (sx + 0.5) / SAMPLES, y + (sy + 0.5) / SAMPLES, poly):
                        hits += 1
            row.append(hits / (SAMPLES * SAMPLES) >= COVERAGE)
        rows.append(row)
    # The geometry and the sample grid are both symmetric about the vertical
    # axis, so this is a no-op — asserted rather than trusted.
    for y in range(px):
        for x in range(px):
            assert rows[y][x] == rows[y][px - 1 - x], f'{px}px asymmetry at {x},{y}'
    return rows


def rasterize_svg(path, px):
    png = subprocess.run(['rsvg-convert', '-w', str(px), '-h', str(px), path],
                         capture_output=True, check=True).stdout
    img = Image.open(io.BytesIO(png)).convert('RGBA')
    bg = Image.new('RGBA', img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    grey = bg.convert('L')
    pix = grey.load()
    return [[pix[x, y] < THRESHOLD for x in range(px)] for y in range(px)]


def rotate_ccw(rows, px):
    """new[r][c] = old[c][px-1-r]: the storage rotation the blit cancels."""
    return [[rows[c][px - 1 - r] for c in range(px)] for r in range(px)]


def pack(rows, px):
    stride = (px + 7) // 8
    data = []
    for y in range(px):
        for xb in range(stride):
            byte = 0
            for b in range(8):
                x = xb * 8 + b
                if x >= px or not rows[y][x]:
                    byte |= 0x80 >> b  # 1 = blank, 0 = ink, as gen_icons packs
            data.append(byte)
    return data, stride


def emit(name, upright, px, parts):
    stored = rotate_ccw(upright, px)
    data, stride = pack(stored, px)
    art = '\n'.join('// ' + ''.join('#' if c else '.' for c in row) for row in upright)
    hexes = ', '.join(f'0x{b:02X}' for b in data)
    parts.append(
        f'\n// As displayed (the stored bytes are this, pre-rotated 90° CCW):\n{art}\n'
        f'static const uint8_t {name}_bits[] = {{{hexes}}};\n'
        f'static const freeink::Icon {name} = {{{px}, {px}, {stride}, {name}_bits}};\n'
    )


def main():
    parts = [
        '#pragma once\n\n#include "Icon.h"\n\n'
        '// Generated by scripts/gen_star_icon.py. Do not edit.\n'
        '// Stored pre-rotated 90° CCW because the raw drawIcon blit displays\n'
        '// bitmaps rotated 90° CW on the portrait X3; see the script.\n'
    ]
    for px in STAR_SIZES:
        emit(f'icon_star_{px}', rasterize_star(px), px, parts)
    emit('icon_book_upright_24', rasterize_svg(BOOK_SVG, 24), 24, parts)
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(''.join(parts))
    print(f'wrote {OUT}')


if __name__ == '__main__':
    main()
