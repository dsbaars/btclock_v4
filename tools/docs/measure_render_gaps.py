#!/usr/bin/env python3
"""Measure pixel gaps between text/divider ink groups in BTClock doc renders.

For comparing layout/spacing between two PNGs under docs/img/screens or
docs/img/fonts (e.g. before/after a font or splitter change).

The renders use a card "white" of RGB(218,219,222) (~grey 219), so we
threshold ink at L<120 and card-interior at L>=200.

Usage:
    tools/docs/measure_render_gaps.py [--axis y|x] [--thr N] PNG [PNG ...]

  --axis y   measure ink groups along y (default; for non-rotated text)
  --axis x   measure along x (for rotated/vertical labels)
  --thr N    ink threshold (default 120)

The script auto-locates the leftmost label/card on the image (the small
left-side cell on screen renders) and reports ink-group bounds and
inter-group gaps inside that cell. Pass multiple PNGs to compare side by side.

Requires Pillow only — runs in the docs venv at .venv-docs without numpy.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image


CARD_BRIGHT = 200  # card "white" is ~219; treat >=200 as card interior
INK_DARK = 120     # ink (text/divider) threshold


def find_leftmost_card(im):
    """Return (x0, x1, y0, y1) bounding box of the leftmost wide bright cell.

    Samples a row at y=200 (above the divider line for split labels) to find
    horizontal bright runs >80 px wide, then walks vertically at the cell's
    x-center to find its full y-extent.
    """
    w, h = im.size
    px = im.load()
    # Try a few sample rows in case the layout differs
    for ys in (200, 180, 220, h // 4, 3 * h // 4):
        runs = []
        in_run = False
        s = 0
        for x in range(w):
            if px[x, ys] >= CARD_BRIGHT:
                if not in_run:
                    s = x
                    in_run = True
            else:
                if in_run:
                    runs.append((s, x - 1))
                    in_run = False
        if in_run:
            runs.append((s, w - 1))
        wide = [r for r in runs if (r[1] - r[0]) > 80]
        if wide:
            xL, xR = wide[0]
            xc = (xL + xR) // 2
            # Walk y at xc and find min/max y where it's bright
            y_runs = []
            in_run = False
            s = 0
            for y in range(h):
                if px[xc, y] >= CARD_BRIGHT:
                    if not in_run:
                        s = y
                        in_run = True
                else:
                    if in_run:
                        y_runs.append((s, y - 1))
                        in_run = False
            if in_run:
                y_runs.append((s, h - 1))
            if y_runs:
                y_top = min(r[0] for r in y_runs)
                y_bot = max(r[1] for r in y_runs)
                return xL, xR, y_top, y_bot
    raise RuntimeError("could not locate a bright card on this image")


def ink_groups(im, x0, x1, y0, y1, axis, thr):
    """Return list of (start, end) ink groups along the chosen axis."""
    px = im.load()
    indices = []
    if axis == "y":
        for y in range(y0, y1 + 1):
            cnt = sum(1 for x in range(x0, x1 + 1) if px[x, y] < thr)
            if cnt > 0:
                indices.append(y)
    else:  # axis == "x"
        for x in range(x0, x1 + 1):
            cnt = sum(1 for y in range(y0, y1 + 1) if px[x, y] < thr)
            if cnt > 0:
                indices.append(x)
    groups = []
    if indices:
        s = p = indices[0]
        for v in indices[1:]:
            if v == p + 1:
                p = v
            else:
                groups.append((s, p))
                s = p = v
        groups.append((s, p))
    return groups


def report(path, axis, thr, margin=4):
    im = Image.open(path).convert("L")
    xL, xR, yT, yB = find_leftmost_card(im)
    x0, x1 = xL + margin, xR - margin
    y0, y1 = yT + margin, yB - margin
    groups = ink_groups(im, x0, x1, y0, y1, axis, thr)
    print(f"\n=== {path}")
    print(f"    card x={xL}..{xR} y={yT}..{yB}  axis={axis} thr<{thr}")
    if not groups:
        print("    (no ink found)")
        return
    print("    ink groups:")
    for i, g in enumerate(groups):
        print(f"      [{i}] {axis}={g[0]:4d}..{g[1]:4d}  size={g[1] - g[0] + 1}")
    print("    gaps:")
    for i in range(len(groups) - 1):
        gap = groups[i + 1][0] - groups[i][1] - 1
        print(f"      gap[{i}->{i + 1}] = {gap} px")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--axis", choices=("y", "x"), default="y",
                    help="measurement axis: y for horizontal text, x for rotated/vertical labels")
    ap.add_argument("--thr", type=int, default=INK_DARK, help="ink threshold (default 120)")
    ap.add_argument("paths", nargs="+", help="PNG file(s) to analyze")
    args = ap.parse_args(argv)
    for p in args.paths:
        if not Path(p).exists():
            print(f"missing: {p}", file=sys.stderr)
            sys.exit(1)
        report(p, axis=args.axis, thr=args.thr)


if __name__ == "__main__":
    main()
