#!/usr/bin/env python3
"""Largeur en pixels d'une chaine dans une police integree du firmware.

Somme les advanceX (virgule fixe 12.4) lus dans lib/EpdFont/builtinFonts/<face>.h.
Ignore le crenage (quelques px au plus). Signale tout codepoint ABSENT de la
police : EpdFont.cpp le saute en silence au rendu, sans marqueur.

  measure_label.py <face> "chaine" [--max N]
  measure_label.py inter_8_regular "Impossible d'enregistrer" --max 200
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1] / "lib/EpdFont/builtinFonts"
# Resolution identique a EpdFont::getGlyph : on ne lit PAS les commentaires
# (imprimables commentes "// A", les autres "// U+00E9"), on suit la table
# d'intervalles vers l'index du glyphe, dans l'ordre du tableau.
GLYPH = re.compile(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}")
INTERVAL = re.compile(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}")

def _block(text, marker):
    start = text.index(marker)
    return text[start:text.index("};", start)]

def load(face):
    path = ROOT / f"{face}.h"
    if not path.exists():
        sys.exit(f"police introuvable : {path}")
    text = path.read_text(encoding="utf-8")
    glyphs = [int(m.group(3)) for m in GLYPH.finditer(_block(text, f"{face}Glyphs[]"))]
    intervals = [(int(a, 16), int(b, 16), int(o, 16))
                 for a, b, o in INTERVAL.findall(_block(text, f"{face}Intervals[]"))]
    def adv(cp):
        for first, last, off in intervals:
            if first <= cp <= last:
                i = off + (cp - first)
                return glyphs[i] if i < len(glyphs) else None
        return None
    return adv

def width(face, text):
    adv = load(face)
    total, missing = 0, []
    for ch in text:
        a = adv(ord(ch))
        if a is None:
            missing.append(f"U+{ord(ch):04X} {ch!r}")
        else:
            total += a
    return total / 16.0, missing

if __name__ == "__main__":
    args = sys.argv[1:]
    limit = None
    if "--max" in args:
        i = args.index("--max"); limit = float(args[i + 1]); del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    w, missing = width(args[0], args[1])
    verdict = ""
    if limit is not None:
        verdict = "  OK" if w <= limit else f"  DEBORDE de {w - limit:.1f} px"
    print(f"{w:7.1f} px  {args[0]:22s} {args[1]!r}{verdict}")
    for m in missing:
        print(f"         ABSENT de la police (saute en silence) : {m}")
    sys.exit(1 if missing or (limit is not None and w > limit) else 0)
