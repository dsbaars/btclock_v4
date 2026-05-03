# Embedded TTFs

## Antonio / AntonioSemiBold / AntonioBold / Oswald

Subsetted with kerning/ligature tables stripped. Antonio additionally
includes the common currency symbols used by the price screen (£, ¥, €);
Oswald is ASCII-only since it only renders the split-text label.

The upstream Antonio TTF is a variable font with a `wght` axis spanning
100..700 and named instances at Thin (100), Light (300), Regular (400),
SemiBold (600), and Bold (700). stb_truetype doesn't read fvar/gvar/HVAR
— it always renders the variable font's default master — so all three
in-tree Antonio cuts are baked as static instances before subsetting.
Instancing also drops the variable tables (gvar accounts for ~11 KB),
saving ~13 KB versus shipping the variable subset.

To regenerate from upstream Google Fonts sources:

```sh
curl -L -o /tmp/Antonio-VF.ttf "https://github.com/google/fonts/raw/main/ofl/antonio/Antonio%5Bwght%5D.ttf"
curl -L -o /tmp/Oswald.ttf     "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf"

# Instance the variable font at each weight we ship, then subset.
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=400 \
    -o /tmp/Antonio-Regular.ttf
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=600 \
    -o /tmp/Antonio-SemiBold.ttf
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=700 \
    -o /tmp/Antonio-Bold.ttf
pyftsubset /tmp/Antonio-Regular.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Antonio.ttf
pyftsubset /tmp/Antonio-SemiBold.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=AntonioSemiBold.ttf
pyftsubset /tmp/Antonio-Bold.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=AntonioBold.ttf

pyftsubset /tmp/Oswald.ttf \
    --unicodes=U+0020-007E \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Oswald.ttf
```

Antonio codepoints: printable ASCII + £ (U+00A3) + ¥ (U+00A5) + € (U+20AC).
`$` (U+0024) is already part of ASCII. Add more symbols here as more
currencies are wired through to the price screen.

Both licensed OFL (SIL Open Font License), compatible with the project's
Apache-2.0 firmware licence.

## Inter / InterBold

Inter is a contemporary sans typeface (rsms / Rasmus Andersson). Used
as a selectable family alongside Antonio / Oswald so users can pick a
modern sans for the main panels. The text cut (vs. the Display cut) is
wider and more open at the BTClock's rendered sizes, which legibility-
tested better than Inter Display did on the e-paper panels.

Sourced from the upstream Inter 4.1 release (rsms/inter on GitHub),
specifically the `extras/ttf/Inter-Regular.ttf` and `Inter-Bold.ttf`
payloads.

```sh
pyftsubset Inter-4.1/extras/ttf/Inter-Regular.ttf \
    --output-file=Inter.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset Inter-4.1/extras/ttf/Inter-Bold.ttf \
    --output-file=InterBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Subset range matches Antonio: printable ASCII (U+0020..U+007E) plus the
three currency symbols the price screen renders — £ (U+00A3), ¥
(U+00A5), € (U+20AC). `$` (U+0024) is already in ASCII.

Hinting is dropped (`--no-hinting`) because the renderer rasterises at
fixed pixel sizes well above where TrueType hinting helps; this trims
~30% off the resulting subset.

Licensed under SIL Open Font License 1.1 — compatible with the project's
Apache-2.0 firmware licence. See upstream `LICENSE.txt` in the Inter 4.1
distribution.

## SourceSerif / SourceSerifBold

Source Serif 4 (Adobe). A modern transitional serif designed for
on-screen reading at body sizes. Selectable family alongside the sans
options.

Sourced from the upstream `adobe-fonts/source-serif` release TTF payload:

```sh
curl -L -o /tmp/SourceSerif4-Regular.ttf \
    "https://github.com/adobe-fonts/source-serif/raw/release/TTF/SourceSerif4-Regular.ttf"
curl -L -o /tmp/SourceSerif4-Bold.ttf \
    "https://github.com/adobe-fonts/source-serif/raw/release/TTF/SourceSerif4-Bold.ttf"

pyftsubset /tmp/SourceSerif4-Regular.ttf \
    --output-file=SourceSerif.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/SourceSerif4-Bold.ttf \
    --output-file=SourceSerifBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1.

## Merriweather / MerriweatherBold

Merriweather (SorkinType, originally Eben Sorkin). A slab-leaning serif
designed for high readability on screens — chunkier strokes than Source
Serif, holds up well at small sizes on e-paper.

Sourced from the upstream SorkinType repo's static TTF payload (the
upstream variable-font workflow lands in `fonts/ttf/`):

```sh
curl -L -o /tmp/Merriweather-Regular.ttf \
    "https://github.com/SorkinType/Merriweather/raw/master/fonts/ttf/Merriweather-Regular.ttf"
curl -L -o /tmp/Merriweather-Bold.ttf \
    "https://github.com/SorkinType/Merriweather/raw/master/fonts/ttf/Merriweather-Bold.ttf"

pyftsubset /tmp/Merriweather-Regular.ttf \
    --output-file=Merriweather.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Merriweather-Bold.ttf \
    --output-file=MerriweatherBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1.

## Bitter / BitterBold

Bitter (Huerta Tipográfica). A contemporary slab serif, contrast-rich
and sharp. The user-facing name "Bitter HT" refers to the designer
(HT = Huerta Tipográfica) — this is the canonical Bitter.

The upstream `solmatas/BitterPro` repo only ships a variable font
(`fonts/variable/Bitter[wght].ttf`), so we instance it at wght=400 and
wght=700 with `fonttools varLib.instancer` before subsetting. This
matches the static-font output Google Fonts publishes downstream while
keeping the upstream provenance traceable.

```sh
curl -L -o /tmp/Bitter-VF.ttf \
    "https://github.com/solmatas/BitterPro/raw/master/fonts/variable/Bitter%5Bwght%5D.ttf"

python3 -m fontTools.varLib.instancer /tmp/Bitter-VF.ttf wght=400 \
    -o /tmp/Bitter-Regular.ttf
python3 -m fontTools.varLib.instancer /tmp/Bitter-VF.ttf wght=700 \
    -o /tmp/Bitter-Bold.ttf

pyftsubset /tmp/Bitter-Regular.ttf \
    --output-file=Bitter.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Bitter-Bold.ttf \
    --output-file=BitterBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1.

## Atkinson / AtkinsonBold

Atkinson Hyperlegible (Braille Institute of America). Purpose-built for
low-vision readability — distinctive letterforms, generous x-height,
disambiguated glyph pairs (e.g. `1 / l / I`, `0 / O`). Used as the body-
text face for `debug` and `provisioning_ui` screens (where DejaVu used
to render preformatted lines), and selectable as a panel font.

```sh
curl -L -o /tmp/AtkinsonHyperlegible-Regular.ttf \
    "https://github.com/googlefonts/atkinson-hyperlegible/raw/main/fonts/ttf/AtkinsonHyperlegible-Regular.ttf"
curl -L -o /tmp/AtkinsonHyperlegible-Bold.ttf \
    "https://github.com/googlefonts/atkinson-hyperlegible/raw/main/fonts/ttf/AtkinsonHyperlegible-Bold.ttf"

pyftsubset /tmp/AtkinsonHyperlegible-Regular.ttf \
    --output-file=Atkinson.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/AtkinsonHyperlegible-Bold.ttf \
    --output-file=AtkinsonBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1, with the Atkinson Hyperlegible
addendum reserving the family name (does not affect the subset's
legality).

## SatoshiSymbol

Private-Use Area font carrying 16 variants of the Bitcoin/sats
symbol at codepoints **U+E000..U+E00F**, used by the price and
Moscow-time screens to render the "sat" glyph inline with Antonio
digits.

Origin: extracted from a Font Awesome Kit, preserved here as
`SatoshiSymbol_source.woff2` (2168 B) for reproducibility. The kit
metadata identifies it as `Font Awesome Kit Regular-5.15.4`;
copyright reads "Copyrights may held by the various creators of the
icons in this font" — treat as indeterminate and keep the source
woff2 alongside the TTF so provenance is traceable.

Default variant: **U+E007** — chosen for visual balance against
Antonio digits. The active variant is NVS-configurable per beads
issue lx0.11.

### U+E007 side-bearings (historical)

The source U+E007 glyph ships with **zero side-bearings**
(advance=324 em, bbox xMin=0..xMax=323 on upem=512). Our TTF in-tree
has been fontTools-patched to advance=420, lsb=48, shifting the outline
+48 em on x — the change is preserved here as a historical artifact.

**It has no visual effect with the current renderer.** Single-glyph
panels go through `DrawTextCentered` which ink-centres the glyph
inside the panel and explicitly undoes any left side-bearing
(`x_origin = (panel_w - ink_w) / 2 - left_bearing`). Visual padding
for the sats glyph — i.e. making the gap to the neighbouring Antonio
digit match the digit-to-digit gap — is instead handled at render
time in `main/screens/moscow_time.cpp`, via the `x_offset_px`
parameter added to `DrawTextCentered`. See that file for the shift
computation.

If the renderer is ever reworked to honour font-level advance/lsb
(e.g. for multi-glyph flow layouts), the font-level sidebearings
become load-bearing; leaving the patched metrics in place now costs
nothing and keeps the option open.

### Conversion procedure

```sh
# woff2 -> ttf, drop layout tables, keep all 16 PUA glyphs
pyftsubset SatoshiSymbol_source.woff2 \
    --unicodes=U+E000-E00F \
    --drop-tables+=GPOS,GSUB,DSIG \
    --flavor= \
    --output-file=SatoshiSymbol.ttf

# then apply the U+E007 padding patch (see commit history for the
# fontTools script)
```

License: see upstream Font Awesome 5.15.4 kit — icon-origin
copyrights are indeterminate per the embedded name-table notice.
The `SatoshiSymbol_source.woff2` is retained in-tree so downstream
licence review can trace the artifact.

## MaterialDesignIcons

Subsetted from `Templarian/MaterialDesign-Webfont` on GitHub. Only the
handful of glyphs the firmware actually paints (Bitaxe, mining pool
stats, nostr zap notification screens) are kept — the full upstream
font is ~1.3 MB with 10k+ glyphs. The subsetted bundle is regenerated
by `tools/fonts/regen_mdi.sh`, which reads the list of icons from the
script itself and parses the upstream `materialdesignicons.css` to
resolve each icon name to its codepoint.

The codepoint constants exposed to C++ live in
`components/fonts/include/mdi_codepoints.hpp` (auto-generated
alongside the TTF).

Licensed under Apache-2.0 by Pictogrammers / upstream MDI maintainers,
compatible with this repo's Apache-2.0 licence.

## Sizes

In-tree byte counts (`wc -c`), rounded:

| Font                  | Size    |
|-----------------------|--------:|
| Antonio               |   9.6 KB |
| AntonioSemiBold       |   9.6 KB |
| AntonioBold           |   9.6 KB |
| Oswald                |   8.9 KB |
| OswaldBold            |   8.9 KB |
| Inter                 |   9.6 KB |
| InterBold             |   9.4 KB |
| SourceSerif           |  11.5 KB |
| SourceSerifBold       |  11.4 KB |
| Merriweather          |  11.8 KB |
| MerriweatherBold      |  11.6 KB |
| Bitter                |  11.0 KB |
| BitterBold            |  11.0 KB |
| Atkinson              |   9.0 KB |
| AtkinsonBold          |   8.8 KB |
| SatoshiSymbol         |   3.5 KB |
| MaterialDesignIcons   |   1.0 KB |

Subset extents — every selectable family (Antonio, Oswald, Inter,
SourceSerif, Merriweather, Bitter, Atkinson) carries printable ASCII
(U+0020..U+007E) plus £/¥/€ (U+00A3, U+00A5, U+20AC) so the price
screen's currency symbol set works regardless of the chosen `fontName`.

The `SatoshiSymbol_source.woff2` archive (2.1 KB) is kept alongside
the TTF for provenance and is not compiled into firmware.
