# Font tooling

Small scripts used while authoring the font bundle. All operate on files
under `../../components/fonts/assets/`.

| Script | Purpose |
|---|---|
| `inspect_metrics.py` | Dump upem, ascent/descent, cap height, and per-glyph bbox for any TTF. Used when digits / symbols look off-centered and we need to compare scaled metrics against the renderer's geometry. |
| `patch_satoshi_symbol.py` | Apply (or revert) the U+E007 sidebearing patch on `SatoshiSymbol.ttf` — advance 324→420 em, +48 em outline shift. Historical artifact: it's visually inert with the current renderer but preserved so the TTF stays load-bearing if the renderer is ever reworked to honour font advance/lsb. |
| `regen.sh` | Re-pull every selectable family (Antonio/Oswald from Google Fonts main, Source Serif 4 from `adobe-fonts/source-serif`, Merriweather from `SorkinType/Merriweather`, Bitter from `solmatas/BitterPro` — variable font instanced at wght=400/700, Atkinson Hyperlegible from `googlefonts/atkinson-hyperlegible`), re-run `pyftsubset` with the codepoint range actually used (ASCII + £/¥/€), and rebuild `SatoshiSymbol.ttf` from the preserved woff2. Idempotent — the output is byte-identical to the in-tree TTFs as long as upstream hasn't changed. |
| `subset_mdi.py` | Download Material Design Icons (TTF + CSS from the Templarian/MaterialDesign-Webfont GitHub repo), pick out a handful of named icons, and emit a subsetted TTF + a `mdi_codepoints.hpp` header with a `constexpr` codepoint for each. CSS is parsed to resolve names like `lightning-bolt` → U+F140B. |
| `regen_mdi.sh` | Thin wrapper over `subset_mdi.py` with the current list of icons hardcoded. Add an icon name to the `ICONS=()` array, rerun, and the TTF + header regenerate. |

## Requires

- Python 3.10+
- `pip install fonttools`
- `curl` (for `regen.sh`)

The codepoint ranges and licence notes live in
[`../../components/fonts/assets/README.md`](../../components/fonts/assets/README.md) —
this directory only holds the reproducers.
