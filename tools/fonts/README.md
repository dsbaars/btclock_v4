# Font tooling

Small scripts used while authoring the font bundle. All operate on files
under `../../components/fonts/assets/`.

| Script | Purpose |
|---|---|
| `inspect_metrics.py` | Dump upem, ascent/descent, cap height, and per-glyph bbox for any TTF. Used when digits / symbols look off-centered and we need to compare scaled metrics against the renderer's geometry. |
| `patch_satoshi_symbol.py` | Apply (or revert) the U+E007 sidebearing patch on `SatoshiSymbol.ttf` — advance 324→420 em, +48 em outline shift. Historical artifact: it's visually inert with the current renderer but preserved so the TTF stays load-bearing if the renderer is ever reworked to honour font advance/lsb. |
| `regen.sh` | Re-pull Antonio/Oswald from Google Fonts main and DejaVu from the 2.37 release tarball, re-run `pyftsubset` with the codepoint ranges actually used (ASCII + £/¥/€ for Antonio, ASCII only for the rest), and rebuild `SatoshiSymbol.ttf` from the preserved woff2. Idempotent — the output is byte-identical to the in-tree TTFs as long as upstream hasn't changed. |

## Requires

- Python 3.10+
- `pip install fonttools`
- `curl`, `tar` (for `regen.sh`)

The codepoint ranges and licence notes live in
[`../../components/fonts/assets/README.md`](../../components/fonts/assets/README.md) —
this directory only holds the reproducers.
