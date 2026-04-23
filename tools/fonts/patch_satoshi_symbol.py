#!/usr/bin/env python3
"""Apply the U+E007 sidebearing patch to SatoshiSymbol.ttf.

Reproduces commit e01d118: widens the advance of U+E007 from 324 em
to 420 em and shifts every contour +48 em on x, producing lsb=48,
rsb=49 — symmetric ~11 % margins matching Antonio digits' natural
sidebearings.

NOTE: with the current renderer (DrawLineCentered ink-centres the
glyph and explicitly undoes left_bearing), this patch has **zero
visual effect**. The actual sats-glyph visual padding is handled at
render time in main/screens/moscow_time.cpp via reduced pixel-height.
The patch is preserved so the TTF stays load-bearing if the renderer
ever honours advance/lsb directly (e.g. multi-glyph flow layouts).

Usage:

    ./patch_satoshi_symbol.py components/fonts/assets/SatoshiSymbol.ttf
    ./patch_satoshi_symbol.py SatoshiSymbol.ttf --revert

Idempotent in either direction: the script checks the current advance
and skips if already at the target state.
"""

import argparse
import sys
from pathlib import Path

from fontTools.ttLib import TTFont


TARGET_CODEPOINT = 0xE007
ORIGINAL_ADVANCE = 324
ORIGINAL_LSB = 0
PATCHED_ADVANCE = 420
PATCHED_LSB = 48
SHIFT_X = 48


def shift_glyph_x(glyph, delta: int) -> None:
    """Shift every point of a simple glyph by `delta` on x.

    Handles both simple glyphs (direct coordinates) and composite glyphs
    (adjust component offsets). U+E007 is a simple glyph in the current
    TTF — the composite branch is here for completeness.
    """
    if glyph.numberOfContours > 0:
        for i in range(len(glyph.coordinates)):
            x, y = glyph.coordinates[i]
            glyph.coordinates[i] = (x + delta, y)
        glyph.recalcBounds(None)
    elif glyph.numberOfContours < 0:
        for comp in glyph.components:
            if hasattr(comp, "x"):
                comp.x += delta
        glyph.recalcBounds(None)


def apply_patch(font: TTFont, revert: bool) -> bool:
    cmap = font.getBestCmap()
    name = cmap.get(TARGET_CODEPOINT)
    if name is None:
        print(f"U+{TARGET_CODEPOINT:04X} not in cmap — nothing to patch",
              file=sys.stderr)
        return False

    hmtx = font["hmtx"]
    glyf = font["glyf"]
    glyph = glyf[name]
    cur_adv, cur_lsb = hmtx[name]

    if revert:
        target_adv, target_lsb, shift = ORIGINAL_ADVANCE, ORIGINAL_LSB, -SHIFT_X
        label = "revert"
    else:
        target_adv, target_lsb, shift = PATCHED_ADVANCE, PATCHED_LSB, SHIFT_X
        label = "patch"

    if cur_adv == target_adv and cur_lsb == target_lsb:
        print(f"{label}: already at target (advance={cur_adv}, lsb={cur_lsb}) "
              "— no-op")
        return False

    print(f"{label}: advance {cur_adv} -> {target_adv}, "
          f"lsb {cur_lsb} -> {target_lsb}, shift x by {shift:+d}")
    shift_glyph_x(glyph, shift)
    hmtx[name] = (target_adv, target_lsb)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch/revert SatoshiSymbol.ttf U+E007")
    ap.add_argument("font", type=Path, help="path to SatoshiSymbol.ttf")
    ap.add_argument("--revert", action="store_true",
                    help="restore the original advance=324 lsb=0 metrics")
    args = ap.parse_args()

    if not args.font.exists():
        print(f"font not found: {args.font}", file=sys.stderr)
        return 1

    font = TTFont(str(args.font))
    if apply_patch(font, args.revert):
        font.save(str(args.font))
        print(f"saved {args.font}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
