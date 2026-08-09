#!/usr/bin/env python3
"""Offline side-by-side comparator for reader-font anti-aliasing.

Renders sample text through exactly the pipeline the device uses:

    FreeType 8-bit alpha
      -> 4-bit                        (fontconvert.py:352-367)
      -> 2 bpp threshold ladder       (fontconvert.py:380-390)
      -> renderer level mapping       (GfxRenderer.cpp:865-883)
      -> panel's MEASURED grey levels (BitmapHelpers.h:147-161)

and writes one PNG with a row per variant, an x8 zoom strip, and three metrics.

This is a developer tool. It is not referenced by the firmware build and ships
nothing to the device.

Deliberate simplification: kerning is omitted. fontconvert.py extracts GPOS kern
pairs; reproducing that here would add nothing because every variant is laid out
identically, so the comparison between variants is unaffected.

Usage:
    python3 preview_aa.py --font 'LexendDeca[wght].ttf' --variable
    python3 preview_aa.py --font ../builtinFonts/source/LexendDeca/LexendDeca-Regular.ttf
"""

import argparse
import os
import struct
import sys
import tempfile
import zlib

try:
    import freetype
except ImportError:
    sys.exit("preview_aa: freetype-py missing. pip install -r requirements.txt")

# --- The device's constants -------------------------------------------------

# Panel response, measured on X4. lib/GfxRenderer/BitmapHelpers.h:147-161.
# Indexed by the 2bpp raw level fontconvert emits: 0=white .. 3=black.
PANEL = (210, 80, 30, 15)
PAPER, INK = PANEL[0], PANEL[3]
DPI = 150  # fontconvert.py:314 -- set_char_size(pt<<6, pt<<6, 150, 150)

SAMPLE = "Portez ce vieux whisky au juge blond qui fume"
ZOOM_GLYPHS = "omke"
ZOOM = 8
STEM_GLYPHS = "klh"

WIDTH = 1400


class Variant:
    def __init__(self, vid, label, thresholds, weight=400, autohint=False):
        self.vid = vid
        self.label = label
        self.thresholds = thresholds
        self.weight = weight
        self.autohint = autohint

    @property
    def load_flags(self):
        flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_BITMAP
        if self.autohint:
            flags |= freetype.FT_LOAD_FORCE_AUTOHINT
        return flags


# `weight` is an OFFSET from the family's own baseline, not an absolute value.
# Bitter already ships at wght 500 on the SD path (commit cff54d77, "washed out
# ... thinner stems"), so comparing it against 400 would measure that old fix
# rather than this one. --base-weight sets the family's baseline.
VARIANTS = [
    Variant("V0", "ACTUEL  --darken-aa (3,6,10)", (3, 6, 10)),
    Variant("V1", "upstream CrossPoint (4,8,12)", (4, 8, 12)),
    Variant("V2", "halo allege         (5,6,10)", (5, 6, 10)),
    Variant("V3", "halo + milieu       (5,8,10)", (5, 8, 10)),
    Variant("V4", "colorimetrique      (5,13,15)", (5, 13, 15)),
    Variant("V5", "halo+milieu+graisse (5,8,10)", (5, 8, 10), weight=450),
    Variant("V6", "halo+milieu+graisse (5,8,10)", (5, 8, 10), weight=500),
    Variant("V7", "upstream + graisse  (4,8,12)", (4, 8, 12), weight=500),
    Variant("V8", "actuel + autohint   (3,6,10)", (3, 6, 10), autohint=True),
    Variant("V9", "halo+graisse+autohint (5,6,10)", (5, 6, 10), weight=450, autohint=True),
]

BASE_WEIGHT = 400  # overridden by --base-weight


def effective_weight(var):
    return var.weight + BASE_WEIGHT - 400


# --- Font loading -----------------------------------------------------------

_face_cache = {}
_temp_files = []


def load_face(font_path, weight, variable):
    key = (font_path, weight if variable else None)
    if key in _face_cache:
        return _face_cache[key]

    path = font_path
    if variable:
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer

        tt = TTFont(font_path)
        instancer.instantiateVariableFont(
            tt, {"wght": weight}, inplace=True, updateFontNames=False
        )
        fd, path = tempfile.mkstemp(suffix=f"-w{weight}.ttf")
        os.close(fd)
        tt.save(path)
        _temp_files.append(path)

    face = freetype.Face(path)
    _face_cache[key] = face
    return face


# --- Rasterisation, mirroring fontconvert.py --------------------------------


def quantize(alpha8, thresholds):
    """8-bit coverage -> 2bpp raw level. fontconvert.py:352-390.

    The `>> 4` is not incidental: production reduces to 4 bits before
    thresholding, so cutoffs can only land on multiples of 16/255. Reproducing
    that faithfully matters -- thresholding the full 8-bit value here would
    flatter every candidate and hide the real quantisation floor.
    """
    a4 = alpha8 >> 4
    if a4 >= thresholds[2]:
        return 3
    if a4 >= thresholds[1]:
        return 2
    if a4 >= thresholds[0]:
        return 1
    return 0


def raster(face, char, thresholds, flags):
    """Rasterise one character.

    Returns (levels, alpha, left, top, advance_fp4) where `levels` is the 2bpp
    grid the device would store and `alpha` is FreeType's exact area coverage --
    kept so fidelity can be scored against it.

    Uses abs(bitmap.pitch) rather than assuming pitch == width. fontconvert.py
    ignores pitch (a latent trap that fontconvert_sdcard.py:730-746 fixed); any
    FT_LOAD_TARGET_* experiment can return pitch != width and corrupt output
    silently.
    """
    face.load_char(char, flags)
    slot = face.glyph
    bm = slot.bitmap
    w, h, pitch = bm.width, bm.rows, abs(bm.pitch)

    levels = [[0] * w for _ in range(h)]
    alpha = [[0] * w for _ in range(h)]
    for y in range(h):
        base = y * pitch
        for x in range(w):
            a = bm.buffer[base + x]
            alpha[y][x] = a
            levels[y][x] = quantize(a, thresholds)

    # 16.16 -> 12.4, matching fp4_from_ft16_16 (fontconvert.py:204-206).
    advance_fp4 = (slot.linearHoriAdvance + (1 << 11)) >> 12
    return levels, alpha, slot.bitmap_left, slot.bitmap_top, advance_fp4


# --- Canvas -----------------------------------------------------------------


class Canvas:
    def __init__(self, width, height, fill=PAPER):
        self.w = width
        self.h = height
        self.px = bytearray([fill]) * (width * height)

    def set(self, x, y, value):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = value

    def hline(self, x0, x1, y, value):
        for x in range(max(0, x0), min(self.w, x1)):
            self.set(x, y, value)

    def to_png(self, path):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)  # filter type 0 (None)
            raw += self.px[y * self.w : (y + 1) * self.w]

        def chunk(tag, data):
            body = struct.pack(">I", len(data)) + tag + data
            return body + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        with open(path, "wb") as fh:
            fh.write(b"\x89PNG\r\n\x1a\n")
            fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 0, 0, 0, 0)))
            fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
            fh.write(chunk(b"IEND", b""))


def draw_text(canvas, face, text, thresholds, flags, x, baseline):
    pen_fp4 = x << 4
    for ch in text:
        levels, _, left, top, adv = raster(face, ch, thresholds, flags)
        ox = ((pen_fp4 + 8) >> 4) + left
        oy = baseline - top
        for gy, row in enumerate(levels):
            for gx, lv in enumerate(row):
                if lv:
                    canvas.set(ox + gx, oy + gy, PANEL[lv])
        pen_fp4 += adv


def draw_label(canvas, face, text, x, baseline):
    """Labels are drawn hard-black on purpose: they annotate the comparison and
    are not part of it, so they must not be mistaken for sample text."""
    pen_fp4 = x << 4
    for ch in text:
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_BITMAP)
        slot = face.glyph
        bm = slot.bitmap
        pitch = abs(bm.pitch)
        ox = ((pen_fp4 + 8) >> 4) + slot.bitmap_left
        oy = baseline - slot.bitmap_top
        for gy in range(bm.rows):
            for gx in range(bm.width):
                if bm.buffer[gy * pitch + gx] >= 128:
                    canvas.set(ox + gx, oy + gy, 0)
        pen_fp4 += (slot.linearHoriAdvance + (1 << 11)) >> 12


# --- Metrics ----------------------------------------------------------------


def ideal_value(alpha8):
    """What the pixel should look like: FreeType's NORMAL mode already computes
    exact area coverage, so the reference is that coverage blended linearly
    between paper and ink. First-order model -- it assumes the panel's levels are
    linear in reflectance, which is the same assumption the image ditherer's
    calibration was measured under."""
    return PAPER - (PAPER - INK) * alpha8 / 255.0


def score(face, thresholds, flags, text):
    """fidelity = mean |rendered - ideal| over inked pixels, in panel units.
    Lower is better. Measures quantisation fidelity, not outline fidelity: a
    variant that changes the outline (autohint, weight) is scored against its own
    outline, which is what we want -- outline quality is what the eye and the
    stem metric judge."""
    err = 0.0
    n = 0
    ink = 0.0
    glyphs = 0
    for ch in sorted(set(text.replace(" ", ""))):
        levels, alpha, _, _, _ = raster(face, ch, thresholds, flags)
        if not levels:
            continue
        glyphs += 1
        for y, row in enumerate(alpha):
            for x, a in enumerate(row):
                lv = levels[y][x]
                ink += lv / 3.0
                if a == 0 and lv == 0:
                    continue
                err += abs(PANEL[lv] - ideal_value(a))
                n += 1
    if not glyphs or not n:
        return 0.0, 0.0
    return err / n, ink / glyphs


def stem_widths(face, thresholds, flags):
    """Effective stem width of k, l, h -- the measurement from 052f497b, which
    diagnosed 'k' rendering at 2.00px against 2.33px for l and h."""
    widths = {}
    for ch in STEM_GLYPHS:
        levels, _, _, _, _ = raster(face, ch, thresholds, flags)
        if not levels:
            continue
        samples = []
        for y in range(int(len(levels) * 0.2), int(len(levels) * 0.8)):
            row = levels[y]
            x = 0
            while x < len(row) and row[x] == 0:
                x += 1
            if x >= len(row):
                continue
            run = 0.0
            while x < len(row) and row[x] != 0:
                run += row[x] / 3.0
                x += 1
            samples.append(run)
        if samples:
            samples.sort()
            widths[ch] = samples[len(samples) // 2]
    return widths


# --- Sheet ------------------------------------------------------------------


def build(font_path, variable, out_path, sizes=(12, 14)):
    label_face = load_face(font_path, 400, variable)
    label_face.set_char_size(9 << 6, 9 << 6, DPI, DPI)

    # Measure the zoom strip across EVERY variant, not a single probe: weight and
    # hinting both shift bitmap_top by a pixel, and a strip sized from one variant
    # lets a taller one overdraw the row above.
    tops, bottoms = [], []
    for var in VARIANTS:
        probe = load_face(font_path, effective_weight(var), variable)
        probe.set_char_size(14 << 6, 14 << 6, DPI, DPI)
        for ch in ZOOM_GLYPHS:
            levels, _, _, top, _ = raster(probe, ch, var.thresholds, var.load_flags)
            tops.append(top)
            bottoms.append(len(levels) - top)
    zoom_top, zoom_bottom = max(tops), max(bottoms)
    zoom_h = (zoom_top + zoom_bottom) * ZOOM

    head = 44
    row_h = 58 + 34 * len(sizes) + zoom_h + 22
    canvas = Canvas(WIDTH, head + row_h * len(VARIANTS))

    draw_label(
        canvas,
        label_face,
        "Comparateur d'anti-aliasing -- peint avec les niveaux mesures du panneau "
        f"({PANEL[0]} papier / {PANEL[1]} gris clair / {PANEL[2]} gris fonce / {PANEL[3]} noir)."
        "  Fidelite = ecart moyen a l'ideal, plus bas = mieux.",
        24,
        26,
    )

    y = head
    for var in VARIANTS:
        canvas.hline(0, WIDTH, y, 140)
        face = load_face(font_path, effective_weight(var), variable)
        flags = var.load_flags

        face.set_char_size(14 << 6, 14 << 6, DPI, DPI)
        fidelity, ink = score(face, var.thresholds, flags, SAMPLE)
        stems = stem_widths(face, var.thresholds, flags)
        spread = (max(stems.values()) - min(stems.values())) if len(stems) > 1 else 0.0

        draw_label(canvas, label_face, f"{var.vid}   {var.label}  poids {effective_weight(var)}", 24, y + 22)
        draw_label(
            canvas,
            label_face,
            f"fidelite {fidelity:5.1f}     encre {ink:5.1f} px/glyphe     "
            f"ecart de fut k/l/h {spread:.2f} px     "
            + " ".join(f"{c}={stems[c]:.2f}" for c in STEM_GLYPHS if c in stems),
            24,
            y + 44,
        )
        row_y = y + 58

        for pt in sizes:
            face.set_char_size(pt << 6, pt << 6, DPI, DPI)
            draw_text(canvas, face, SAMPLE, var.thresholds, flags, 24, row_y + pt + 8)
            row_y += 34

        face.set_char_size(14 << 6, 14 << 6, DPI, DPI)
        baseline = row_y + zoom_top * ZOOM
        zx = 24
        for ch in ZOOM_GLYPHS:
            levels, _, _, top, _ = raster(face, ch, var.thresholds, flags)
            oy = baseline - top * ZOOM
            for gy, row in enumerate(levels):
                for gx, lv in enumerate(row):
                    if not lv:
                        continue
                    value = PANEL[lv]
                    for dy in range(ZOOM):
                        for dx in range(ZOOM):
                            canvas.set(zx + gx * ZOOM + dx, oy + gy * ZOOM + dy, value)
            zx += (len(levels[0]) if levels else 4) * ZOOM + 20
        y += row_h

    canvas.to_png(out_path)
    for path in _temp_files:
        try:
            os.unlink(path)
        except OSError:
            pass
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", required=True, help="TTF path")
    ap.add_argument("--variable", action="store_true", help="source has a wght axis")
    ap.add_argument("--out", default="preview_aa.png")
    ap.add_argument("--base-weight", type=int, default=400,
                    help="family baseline on the wght axis (Bitter ships at 500)")
    args = ap.parse_args()

    global BASE_WEIGHT
    BASE_WEIGHT = args.base_weight

    if not os.path.exists(args.font):
        sys.exit(f"preview_aa: font not found: {args.font}")

    if any(v.weight != 400 for v in VARIANTS) and not args.variable:
        sys.exit(
            "preview_aa: variants request non-400 weights but --variable was not "
            "given. A static face cannot be re-weighted; rendering them all at one "
            "weight would silently invalidate the comparison."
        )

    print(build(args.font, args.variable, args.out))


if __name__ == "__main__":
    main()
