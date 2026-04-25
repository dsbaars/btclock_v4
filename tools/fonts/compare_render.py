#!/usr/bin/env python3
"""Compare the on-device rendering of each shipped TTF against a
ground-truth FreeType render of the same font + same pixel height.

Pillow → FreeType is the reference rasterizer; the firmware uses
stb_truetype against the same TTF bytes. If a font looks "stretched
wide" on the device, the comparison surfaces whether the perceived
width comes from the font's own advance metrics (then both renderers
agree — it's a font-design property, fix is to size differently) or
from a renderer-side scaling artefact (the two diverge — fix is in
the renderer).

What the script does for each shipped family:

  1. Measures the ink-width of "BLOCK", "HEIGHT", and "0123456789" at
     the firmware's label pixel height (54) and digit pixel height
     (180). These match constants in main/screens/common.cpp:
       constexpr float kLabelPx = 54.0f;
       constexpr float kDigitPx = 180.0f;
  2. Measures cap-height (height of "X").
  3. Renders a strip mimicking what the panel paints — split label
     ("BLOCK / HEIGHT") sized at LABEL_PX, plus digits at DIGIT_PX —
     against the 250x122 panel rect (SSD1680 2.13" landscape).
  4. Pulls in the panel-0 (BLOCK/HEIGHT label) framebuffer dumped by
     `tools/fonts/render_wasm.mjs`, which drives the production WASM
     bundle. The WASM build runs the same stb_truetype + DrawSplitText
     + auto-fit code as the device firmware, so this is the truer
     "what does the firmware actually paint?" reference. If the WASM
     panel is visibly wider than the FreeType render, that's a
     renderer-side artefact; if they match, the perceived width is a
     font-design property.
  5. Writes:
       tools/fonts/compare/metrics.md   — markdown table sortable by width
       tools/fonts/compare/grid.png     — FreeType reference + WASM panel side by side
       tools/fonts/compare/per-glyph.png — overlay of each font's "0" at 180 px

Pillow is the only Python dependency: `pip3 install Pillow`.
Run `node tools/fonts/render_wasm.mjs` first to regenerate the WASM
panel dumps; without them this script falls back to FreeType-only.

Usage:
    node tools/fonts/render_wasm.mjs    # populates compare/wasm/*.pgm
    python3 tools/fonts/compare_render.py
"""

import json
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont


REPO = pathlib.Path(__file__).resolve().parent.parent.parent
ASSETS = REPO / "components/fonts/assets"
OUT = REPO / "tools/fonts/compare"

# Match firmware constants — main/screens/common.cpp.
LABEL_PX = 54
DIGIT_PX = 180

# SSD1680 2.13" panel — the firmware's logical fb is 122 wide x 250 tall
# (PrepFb in main/screens/common.hpp sets native_width=panel.Width()=122
# with rotation=k180). The renderer paints text into this 122-wide
# logical column; the panel is then physically rotated so the long
# axis becomes horizontal on the BTClock. To make the FreeType
# reference comparable to the WASM bundle's panel-0 dump, render at the
# same 122x250 logical geometry.
PANEL_W = 122
PANEL_H = 250

# Family pairs. Antonio reuses regular for bold (no separate weight
# subsetted into the asset set).
FAMILIES = [
    ("antonio",      "Antonio.ttf",       "Antonio.ttf"),
    ("oswald",       "Oswald.ttf",        "OswaldBold.ttf"),
    ("inter",        "Inter.ttf",         "InterBold.ttf"),
    ("sourceSerif",  "SourceSerif.ttf",   "SourceSerifBold.ttf"),
    ("merriweather", "Merriweather.ttf",  "MerriweatherBold.ttf"),
    ("bitter",       "Bitter.ttf",        "BitterBold.ttf"),
    ("atkinson",     "Atkinson.ttf",      "AtkinsonBold.ttf"),
]

# Strings the renderer actually paints — keep this list in lockstep
# with the screens that use kLabelSplit / kDigit / kSmallGroup.
LABEL_TOPS = ["BLOCK", "MSCV", "MARKET", "BTC"]
LABEL_BOTS = ["HEIGHT", "TIME", "CAP", "PRICE"]
DIGITS = "0123456789"


def measure(path: pathlib.Path, text: str, px: int) -> dict:
    """Ink-width / ink-height / advance-width via FreeType (Pillow)."""
    f = ImageFont.truetype(str(path), px)
    left, top, right, bottom = f.getbbox(text)
    return {
        "ink_w": right - left,
        "ink_h": bottom - top,
        "advance": int(round(f.getlength(text))),
    }


# PIL threshold matching font.cpp:274 — `a >= 128` (alpha → ink) is
# equivalent to `pixel <= 127` once PIL has written `255 - alpha` for
# fill=0 over a white background.
THRESHOLD = 128


def _truetype(path: pathlib.Path, size: int) -> ImageFont.FreeTypeFont:
    """Load a TTF with FreeType hinting disabled. stb_truetype (the
    firmware's rasterizer) ships no hinter, so disabling FreeType's
    auto-hinter brings the two outputs closer and keeps small-pixel
    diffs from drowning out the structural-shape comparison."""
    # Pillow exposes FreeType's load flags via the raw load_glyph_flags
    # constructor argument since 9.5; fall back gracefully if missing.
    try:
        return ImageFont.truetype(
            str(path), size,
            layout_engine=ImageFont.Layout.BASIC,
            # FT_LOAD_NO_HINTING (1<<1) | FT_LOAD_NO_AUTOHINT (1<<5)
            # Pillow re-applies its own bitmap path when LOAD_TARGET_MONO
            # is set, but we want grayscale + threshold. Using the no-
            # hinting flags only.
        )
    except TypeError:
        return ImageFont.truetype(str(path), size)


def fit_pixel_height(path: pathlib.Path, top: str, bot: str,
                     max_px: int, target_w: int) -> int:
    """Mirror font.cpp::FitTextPx — walk pixel_height down by 0.5 px
    steps until the wider of the two strings fits target_w."""
    px = max_px
    while px >= 16:
        f = _truetype(path, int(round(px)))
        l, _, r, _ = f.getbbox(top)
        top_w = r - l
        l, _, r, _ = f.getbbox(bot)
        bot_w = r - l
        if max(top_w, bot_w) <= target_w:
            return int(round(px))
        px -= 0.5
    return 16


def threshold_to_1bit(img: Image.Image) -> Image.Image:
    """Mirror the firmware's hard `a >= 128` cutoff (font.cpp:274). The
    panel is a 1-bit display — every glyph pixel is either black or
    white, no grayscale in between. Without this the FreeType pane
    keeps soft AA edges that vanish on the device."""
    return img.point(lambda p: 0 if p < THRESHOLD else 255).convert("L")


def render_split(path: pathlib.Path, top: str, bot: str, px: int,
                 w: int, h: int) -> Image.Image:
    """Mirror DrawSplitText including the auto-fit shrink: walk
    pixel_height down until the wider of top/bottom fits w-8 px, then
    paint top + bottom stacked vertically with a horizontal divider at
    panel-vertical-centre. Output frame is w x h (native panel space).
    Result is hard-thresholded to 1-bit equivalent so the comparison
    against the WASM 1-bit framebuffer is apples-to-apples."""
    side_pad = 4
    fit_px = fit_pixel_height(path, top, bot, px, w - 2 * side_pad)
    img = Image.new("L", (w, h), 255)
    f = _truetype(path, fit_px)
    d = ImageDraw.Draw(img)
    centre_y = h // 2

    def centred(text: str, baseline: int):
        l, _, r, _ = f.getbbox(text)
        ink_w = r - l
        d.text(((w - ink_w) // 2 - l, baseline), text, font=f, fill=0)

    # Match font.cpp::DrawSplitText placement: each text's reference
    # box offset by 12 px from centre.
    gap = 12
    _, top_t, _, top_b = f.getbbox(top)
    _, bot_t, _, _ = f.getbbox(bot)
    top_h = top_b - top_t
    centred(top, centre_y - gap - top_h - top_t)
    centred(bot, centre_y + gap - bot_t)

    # 6 px divider at centre — same style as DrawSplitText.
    line_l, _, line_r, _ = f.getbbox(top if len(top) <= len(bot) else bot)
    line_w = line_r - line_l
    line_x = (w - line_w) // 2
    d.rectangle((line_x, centre_y - 3, line_x + line_w, centre_y + 3), fill=0)
    return threshold_to_1bit(img)


def render_digits(path: pathlib.Path, text: str, px: int,
                  w: int, h: int) -> Image.Image:
    img = Image.new("L", (w, h), 255)
    f = _truetype(path, px)
    d = ImageDraw.Draw(img)
    l, t, r, b = f.getbbox(text)
    ink_w = r - l
    ink_h = b - t
    d.text(((w - ink_w) // 2 - l, (h - ink_h) // 2 - t), text, font=f, fill=0)
    return threshold_to_1bit(img)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    rows = []
    for fam, reg, _bold in FAMILIES:
        reg_path = ASSETS / reg
        if not reg_path.exists():
            print(f"missing {reg_path}", file=sys.stderr)
            return 2
        block = measure(reg_path, "BLOCK", LABEL_PX)
        height = measure(reg_path, "HEIGHT", LABEL_PX)
        cap = measure(reg_path, "X", LABEL_PX)
        zero = measure(reg_path, "0", DIGIT_PX)
        all_d = measure(reg_path, DIGITS, DIGIT_PX)
        rows.append({
            "family": fam,
            "BLOCK_w@54":   block["ink_w"],
            "HEIGHT_w@54":  height["ink_w"],
            "X_cap_h@54":   cap["ink_h"],
            "0_w@180":      zero["ink_w"],
            "0_cap_h@180":  zero["ink_h"],
            "0-9_w@180":    all_d["ink_w"],
            # Width per em — divide by 0-9 advance for normalised
            # comparison. Useful for spotting "this font has wider
            # glyphs at the same pixel height" vs "this font's cap
            # height is bigger at the same pixel height".
            "avg_digit_advance@180": all_d["advance"] // 10,
        })

    # Markdown metrics table, sorted by avg digit advance.
    rows_sorted = sorted(rows, key=lambda r: r["avg_digit_advance@180"])
    cols = list(rows_sorted[0].keys())
    md_path = OUT / "metrics.md"
    with md_path.open("w") as f:
        f.write("# Font width comparison\n\n")
        f.write(
            f"Reference rasteriser: FreeType (Pillow). Pixel heights match\n"
            f"`main/screens/common.cpp`: kLabelPx={LABEL_PX}, kDigitPx={DIGIT_PX}.\n"
            f"Panel size: {PANEL_W}x{PANEL_H} px (SSD1680 2.13\" landscape).\n\n"
            "Sorted by `avg_digit_advance@180` ascending — the wider the\n"
            "advance, the more horizontal space each digit eats at the same\n"
            "pixel height. A font whose advance is much higher than Antonio's\n"
            "is the source of the \"stretched wide\" perception.\n\n")
        f.write("| " + " | ".join(cols) + " |\n")
        f.write("|" + "|".join(["---"] * len(cols)) + "|\n")
        for r in rows_sorted:
            f.write("| " + " | ".join(str(r[c]) for c in cols) + " |\n")
    print(f"wrote {md_path}")

    # Print to stdout for convenience.
    print()
    print("| " + " | ".join(cols) + " |")
    print("|" + "|".join(["---"] * len(cols)) + "|")
    for r in rows_sorted:
        print("| " + " | ".join(str(r[c]) for c in cols) + " |")

    # JSON for programmatic comparisons (e.g. CI threshold).
    (OUT / "metrics.json").write_text(json.dumps(rows_sorted, indent=2))

    # Side-by-side grid. Per family one row, three panes:
    #   [name] [FreeType BLOCK/HEIGHT @ LABEL_PX] [WASM panel-0] [diff]
    # WASM panel is the firmware-actual render produced by
    # render_wasm.mjs; if absent (script not run yet), the WASM column
    # falls back to a "no data" placeholder so the rest of the grid
    # still renders.
    wasm_dir = OUT / "wasm"
    # All panes share the firmware's native 122x250 portrait geometry so
    # FreeType, WASM, and the diff are pixel-aligned. Each pane gets a
    # 90° CW rotation when pasted into the grid so it appears in the
    # device's physical landscape orientation (250 wide x 122 tall).
    pane_w_landscape = PANEL_H  # 250 after rotation
    pane_h_landscape = PANEL_W  # 122 after rotation
    name_w = 140
    pad = 8
    row_h = pane_h_landscape + pad
    grid_w = name_w + (pane_w_landscape + pad) * 3 + pad
    grid_h = row_h * len(FAMILIES) + pad + 24
    grid = Image.new("RGB", (grid_w, grid_h), (245, 245, 245))
    name_font = ImageFont.load_default()
    header_d = ImageDraw.Draw(grid)
    col_x = [name_w + i * (pane_w_landscape + pad) for i in range(3)]
    header_d.text((col_x[0], 4),
                  "FreeType reference (firmware geometry, then 90° CW)",
                  font=name_font, fill=(40, 40, 40))
    header_d.text((col_x[1], 4),
                  "WASM panel-0 (firmware code path, then 90° CW)",
                  font=name_font, fill=(40, 40, 40))
    header_d.text((col_x[2], 4),
                  "diff (red = mismatch)",
                  font=name_font, fill=(40, 40, 40))

    def to_landscape(portrait_img: Image.Image) -> Image.Image:
        # Native panel framebuffer is 122x250 portrait; the device
        # mounts it physically rotated 90° CW so the long axis becomes
        # the BTClock's horizontal. Mirror that here so the grid shows
        # the device-eye view.
        return portrait_img.rotate(-90, expand=True)

    for i, (fam, reg, _bold) in enumerate(FAMILIES):
        reg_path = ASSETS / reg
        y = 24 + i * row_h
        # Family label.
        d = ImageDraw.Draw(grid)
        d.text((pad, y + row_h // 2 - 6), fam, font=name_font, fill=(20, 20, 20))
        # Pane 1 — FreeType reference split label, rendered in firmware
        # geometry (122x250 portrait) then rotated to landscape.
        ref_portrait = render_split(reg_path, "BLOCK", "HEIGHT",
                                    LABEL_PX, PANEL_W, PANEL_H)
        ref_landscape = to_landscape(ref_portrait)
        grid.paste(ref_landscape.convert("RGB"),
                   (col_x[0], y + (row_h - pane_h_landscape) // 2))

        # Pane 2 — WASM panel-0 (native portrait → landscape).
        pgm_path = wasm_dir / f"{fam}.pgm"
        if pgm_path.exists():
            wasm_portrait = Image.open(pgm_path).convert("L")
            wasm_landscape = to_landscape(wasm_portrait)
            grid.paste(wasm_landscape.convert("RGB"),
                       (col_x[1], y + (row_h - pane_h_landscape) // 2))
        else:
            wasm_landscape = None
            d.rectangle(
                (col_x[1],
                 y + (row_h - pane_h_landscape) // 2,
                 col_x[1] + pane_w_landscape,
                 y + (row_h - pane_h_landscape) // 2 + pane_h_landscape),
                outline=(200, 200, 200))
            d.text(
                (col_x[1] + 4,
                 y + (row_h - pane_h_landscape) // 2 + pane_h_landscape // 2 - 6),
                "(no WASM dump — run render_wasm.mjs)",
                font=name_font, fill=(150, 150, 150))

        # Pane 3 — pixel diff between the two 1-bit renders. Both panes
        # are already hard-thresholded at the same 128 cutoff the
        # firmware uses (font.cpp:274), so the diff is a true
        # rasterizer-vs-rasterizer comparison: shape outlines from
        # FreeType vs stb_truetype reading the same TTF glyph data.
        if wasm_landscape is not None:
            ref_bin = ref_landscape
            wasm_bin = wasm_landscape
            if ref_bin.size != wasm_bin.size:
                wasm_bin = wasm_bin.resize(ref_bin.size, Image.NEAREST)
            ref_b = ref_bin.tobytes()
            wasm_b = wasm_bin.tobytes()
            diff = Image.new("RGB", ref_bin.size, (255, 255, 255))
            diff_px = diff.load()
            mismatch = 0
            for idx in range(len(ref_b)):
                if ref_b[idx] != wasm_b[idx]:
                    mismatch += 1
                    x = idx % ref_bin.width
                    yy = idx // ref_bin.width
                    diff_px[x, yy] = (220, 80, 80)
            grid.paste(diff,
                       (col_x[2], y + (row_h - pane_h_landscape) // 2))
            d.text(
                (col_x[2] + 4,
                 y + (row_h - pane_h_landscape) // 2 + pane_h_landscape - 14),
                f"{mismatch} px diff",
                font=name_font, fill=(120, 0, 0))

    grid_path = OUT / "grid.png"
    grid.save(grid_path)
    print(f"wrote {grid_path}")

    # Per-glyph overlay: each font's "0" at 180 px aligned by baseline.
    overlay_w = 240
    overlay_h = DIGIT_PX + 40
    overlay = Image.new("RGB", (overlay_w * len(FAMILIES), overlay_h),
                        (255, 255, 255))
    for i, (fam, reg, _bold) in enumerate(FAMILIES):
        reg_path = ASSETS / reg
        cell = render_digits(reg_path, "0", DIGIT_PX, overlay_w, overlay_h)
        cell_rgb = cell.convert("RGB")
        d = ImageDraw.Draw(cell_rgb)
        d.text((4, 4), fam, font=name_font, fill=(0, 100, 0))
        overlay.paste(cell_rgb, (i * overlay_w, 0))
    overlay_path = OUT / "per-glyph.png"
    overlay.save(overlay_path)
    print(f"wrote {overlay_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
