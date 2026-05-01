#!/usr/bin/env python3
"""Render the 16 sats-symbol variants U+E000..U+E00F into a single
contact-sheet PNG.

Background: SatoshiSymbol.ttf (components/fonts/assets/) ships 16
glyphs in the Private Use Area at codepoints U+E000..U+E00F. Pick one
via the `satsVariant` setting (range 0..15, default 7). Until this
script existed, the only way to preview the variants was to PATCH the
setting and re-render on the device — slow and serial. The contact
sheet renders all 16 in one image so a user can pick visually before
PATCHing.

Drives PIL/FreeType against the same TTF the firmware embeds, so the
glyphs match what the device paints. Output is sized to fit the
docs/img/fonts/ convention (under the 2000 px cap; PX_PER_MM=8 budget).

Usage:

    ./render_sats_variants.py
    ./render_sats_variants.py --out /tmp/sheet.png --cell-px 200
    ./render_sats_variants.py --out docs/img/fonts/sats_variants.png

Requires Pillow with FreeType support. macOS local: `brew install
freetype && pip install pillow`. Linux: `pip install pillow` is
usually enough.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_FONT = REPO_ROOT / "components" / "fonts" / "assets" / "SatoshiSymbol.ttf"
DEFAULT_LABEL_FONT = (
    REPO_ROOT / "components" / "fonts" / "assets" / "Atkinson.ttf"
)
DEFAULT_OUT = REPO_ROOT / "docs" / "img" / "fonts" / "sats_variants.png"

GRID_COLS = 4
GRID_ROWS = 4  # 16 variants
GLYPH_BASE = 0xE000


def render(
    glyph_font_path: Path,
    label_font_path: Path,
    cell_px: int,
    out_path: Path,
    border_px: int = 2,
    label_px: int = 26,
    pad_px: int = 8,
) -> None:
    """Render a 4×4 grid of the 16 sats glyphs with index labels."""
    glyph_font = ImageFont.truetype(str(glyph_font_path), size=cell_px - 2 * pad_px - label_px)
    label_font = ImageFont.truetype(str(label_font_path), size=label_px)

    sheet_w = GRID_COLS * cell_px
    sheet_h = GRID_ROWS * cell_px
    img = Image.new("RGB", (sheet_w, sheet_h), color="white")
    draw = ImageDraw.Draw(img)

    # Draw cell borders, glyph centred in the cell, and the variant
    # index in the bottom-left of each cell so the user can read off
    # the value to PATCH into satsVariant.
    for i in range(16):
        col = i % GRID_COLS
        row = i // GRID_COLS
        x0 = col * cell_px
        y0 = row * cell_px
        x1 = x0 + cell_px - 1
        y1 = y0 + cell_px - 1

        # Cell border (thin grey).
        draw.rectangle([x0, y0, x1, y1], outline=(200, 200, 200), width=border_px)

        # Glyph: PIL's text() with anchor="mm" centres on the given
        # point. Centre vertically a bit above the label band so they
        # don't overlap.
        glyph = chr(GLYPH_BASE + i)
        glyph_cx = x0 + cell_px // 2
        glyph_cy = y0 + (cell_px - label_px - pad_px) // 2 + pad_px
        draw.text(
            (glyph_cx, glyph_cy),
            glyph,
            font=glyph_font,
            fill="black",
            anchor="mm",
        )

        # Variant index label, bottom-left.
        draw.text(
            (x0 + pad_px, y1 - pad_px),
            f"{i}",
            font=label_font,
            fill=(80, 80, 80),
            anchor="lb",
        )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path, optimize=True)
    print(f"wrote {out_path} ({sheet_w}x{sheet_h})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--font",
        type=Path,
        default=DEFAULT_FONT,
        help=f"SatoshiSymbol TTF path (default: {DEFAULT_FONT.relative_to(REPO_ROOT)})",
    )
    ap.add_argument(
        "--label-font",
        type=Path,
        default=DEFAULT_LABEL_FONT,
        help="Font used for the per-cell index labels.",
    )
    ap.add_argument(
        "--cell-px",
        type=int,
        default=200,
        help="Per-cell pixel width/height. Default 200 → 800x800 sheet.",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output path (default: {DEFAULT_OUT.relative_to(REPO_ROOT)})",
    )
    args = ap.parse_args()

    if not args.font.is_file():
        print(f"font not found: {args.font}", file=sys.stderr)
        return 1
    if not args.label_font.is_file():
        print(f"label font not found: {args.label_font}", file=sys.stderr)
        return 1

    render(args.font, args.label_font, args.cell_px, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
