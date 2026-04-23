#!/usr/bin/env python3
"""Dump font vertical metrics + per-glyph bboxes + simulated stb bitmap
boxes. Used when digits / symbols look off-center or mis-scaled.

Prints upem, hhea + OS/2 typo + OS/2 win ascent/descent, cap height,
fsSelection (with the USE_TYPO_METRICS bit 7 called out), and — if
codepoints are given — each glyph's advance, lsb, glyph bbox, and the
simulated `stbtt_GetCodepointBitmapBox` iy0/iy1 at the requested
pixel-height. That last bit mirrors what the on-device rasteriser
produces, so a renderer-vs-geometry discrepancy can be diagnosed by
comparing this output against measurements from a device photo.

Usage:

    ./inspect_metrics.py Antonio.ttf 0 8 '$' EUR
    ./inspect_metrics.py SatoshiSymbol.ttf E007 --pixel-height 130
    ./inspect_metrics.py Antonio.ttf 0 1 2 3 4 5 6 7 8 9 --pixel-height 180

Codepoints can be given as:
  * single printable char                    ($  €  0)
  * 4-char hex string (no prefix)            (E007  20AC  00A3)
  * 3-letter ISO currency code (shortcut)    (EUR  GBP  JPY  USD)
"""

import argparse
import math
import sys
from pathlib import Path

from fontTools.ttLib import TTFont


CURRENCY_SHORTCUTS = {
    "USD": ord("$"),
    "GBP": 0x00A3,
    "JPY": 0x00A5,
    "EUR": 0x20AC,
    "CAD": ord("$"),
    "AUD": ord("$"),
    "SGD": ord("$"),
    "CHF": ord("$"),
}


def parse_codepoint(s: str) -> int:
    if s in CURRENCY_SHORTCUTS:
        return CURRENCY_SHORTCUTS[s]
    if len(s) == 1:
        return ord(s)
    try:
        return int(s, 16)
    except ValueError:
        raise SystemExit(f"can't parse codepoint {s!r}")


def print_vmetrics(font_path: Path, f: TTFont) -> tuple[int, int]:
    """Print font-level metrics. Returns the (ascent, descent) pair that
    stb_truetype would use for ScaleForPixelHeight — i.e. hhea values."""
    head = f["head"]
    hhea = f["hhea"]
    os2 = f.get("OS/2")

    print(f"=== {font_path} ===")
    print(f"  upem = {head.unitsPerEm}")
    print(f"  hhea  ascent/descent/lineGap = "
          f"{hhea.ascent} / {hhea.descent} / {hhea.lineGap}")
    if os2 is not None:
        print(f"  OS/2  typo ascent/descent = "
              f"{os2.sTypoAscender} / {os2.sTypoDescender}")
        print(f"  OS/2  win  ascent/descent = "
              f"{os2.usWinAscent} / {os2.usWinDescent}")
        cap = getattr(os2, "sCapHeight", None)
        if cap is not None:
            print(f"  OS/2  capHeight = {cap}")
        typo_bit = bool(os2.fsSelection & 0x80)
        print(f"  OS/2  fsSelection = 0x{os2.fsSelection:04x}  "
              f"(USE_TYPO_METRICS bit 7 = {typo_bit})")
    cmap = f.getBestCmap()
    print(f"  cmap glyphs = {len(cmap)}")
    return hhea.ascent, hhea.descent


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Dump TTF metrics + simulate stb bitmap-box positioning.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("font", type=Path, help="path to a TTF")
    ap.add_argument("codepoints", nargs="*",
                    help="codepoints to inspect (char, hex, or ISO ccy)")
    ap.add_argument("--pixel-height", "-p", type=float, default=0.0,
                    help="if set, also print stb iy0/iy1 at this height")
    args = ap.parse_args()

    if not args.font.exists():
        print(f"font not found: {args.font}", file=sys.stderr)
        return 1

    f = TTFont(str(args.font))
    ascent, descent = print_vmetrics(args.font, f)

    if not args.codepoints:
        return 0

    cmap = f.getBestCmap()
    hmtx = f["hmtx"]
    glyf = f.get("glyf")

    scale = 0.0
    if args.pixel_height > 0:
        # Matches stbtt_ScaleForPixelHeight: fheight = ascent - descent
        scale = args.pixel_height / (ascent - descent)
        print(f"\npixel_height={args.pixel_height:g}  "
              f"→ scale={scale:.6f}  "
              f"(full em would render at {(ascent - descent) * scale:.1f} px)")

    cols = ["cp", "ch", "glyph", "adv", "lsb",
            "xMin", "xMax", "yMin", "yMax"]
    fmt = "{cp:<7}{ch:<4}{glyph:<12}{adv:>5}{lsb:>5}{xMin:>6}{xMax:>6}{yMin:>6}{yMax:>6}"
    if scale:
        cols += ["iy0", "iy1", "above", "below"]
        fmt += "{iy0:>5}{iy1:>5}{above:>7}{below:>7}"

    print()
    print(fmt.format(**{c: c for c in cols}))
    for raw in args.codepoints:
        cp = parse_codepoint(raw)
        name = cmap.get(cp)
        if name is None:
            print(f"U+{cp:04X}  MISSING")
            continue
        aw, lsb = hmtx[name]
        g = glyf[name] if glyf is not None else None
        if g is not None and g.numberOfContours:
            bb = (g.xMin, g.xMax, g.yMin, g.yMax)
        else:
            bb = (0, 0, 0, 0)
        try:
            ch = chr(cp)
            if not ch.isprintable():
                ch = "?"
        except ValueError:
            ch = "?"
        row = dict(cp=f"U+{cp:04X}", ch=ch, glyph=name, adv=aw, lsb=lsb,
                   xMin=bb[0], xMax=bb[1], yMin=bb[2], yMax=bb[3])
        if scale:
            # Mirror stbtt_GetGlyphBitmapBoxSubpixel:
            #   iy0 = floor(-yMax * scale)   (top of bitmap, negative if above baseline)
            #   iy1 = ceil (-yMin * scale)   (bottom of bitmap, positive if below)
            iy0 = math.floor(-bb[3] * scale)
            iy1 = math.ceil(-bb[2] * scale)
            row["iy0"] = iy0
            row["iy1"] = iy1
            row["above"] = -iy0
            row["below"] = max(0, iy1)
        print(fmt.format(**row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
