# Embedded TTFs

## Antonio / AntonioSemiBold / AntonioBold / Oswald / OswaldBold

Subsetted with kerning/ligature tables stripped. Both families ship the
common currency symbols used by the price screen (£, ¥, €, ₿) — Oswald
is selectable as a price-screen family, not just for the split-text
label, so it needs the same range as Antonio.

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
curl -L -o /tmp/Oswald-VF.ttf  "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf"

# Instance the variable font at each weight we ship, then subset.
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=400 \
    -o /tmp/Antonio-Regular.ttf
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=600 \
    -o /tmp/Antonio-SemiBold.ttf
python3 -m fontTools.varLib.instancer /tmp/Antonio-VF.ttf wght=700 \
    -o /tmp/Antonio-Bold.ttf
pyftsubset /tmp/Antonio-Regular.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Antonio.ttf
pyftsubset /tmp/Antonio-SemiBold.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=AntonioSemiBold.ttf
pyftsubset /tmp/Antonio-Bold.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=AntonioBold.ttf

# Oswald: base cut at 400 / bold role at 700, then subset.
python3 -m fontTools.varLib.instancer /tmp/Oswald-VF.ttf wght=400 \
    -o /tmp/Oswald-Regular.ttf
python3 -m fontTools.varLib.instancer /tmp/Oswald-VF.ttf wght=700 \
    -o /tmp/Oswald-Bold.ttf
pyftsubset /tmp/Oswald-Regular.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Oswald.ttf
pyftsubset /tmp/Oswald-Bold.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=OswaldBold.ttf
```


Antonio codepoints: printable ASCII + £ (U+00A3) + ¥ (U+00A5) + € (U+20AC)
+ ₿ (U+20BF). `$` (U+0024) is already part of ASCII. ₿ is requested
unconditionally; if a given upstream lacks the glyph, pyftsubset's
`--ignore-missing-unicodes` lets the build proceed and the verify pass
in `tools/fonts/regen.sh` flags the gap. Add more symbols here as more
currencies are wired through to the price screen.

Both licensed OFL (SIL Open Font License), compatible with the project's
Apache-2.0 firmware licence.

## Inter / InterBold

Inter is a contemporary sans typeface (rsms / Rasmus Andersson). Used
as a selectable family alongside Antonio / Oswald so users can pick a
modern sans for the main panels. The text cut (vs. the Display cut) is
wider and more open at the BTClock's rendered sizes, which legibility-
tested better than Inter Display did on the e-paper panels.

Sourced from the upstream Inter 4.1 release (rsms/inter on GitHub).
BTClock uses **SemiBold as the family base cut** for `Inter.ttf`
(to improve e-paper legibility), while `InterBold.ttf` remains the
markdown bold role cut.

```sh
pyftsubset Inter-4.1/extras/ttf/Inter-SemiBold.ttf \
    --output-file=Inter.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset Inter-4.1/extras/ttf/Inter-Bold.ttf \
    --output-file=InterBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Subset range matches Antonio: printable ASCII (U+0020..U+007E) plus the
currency symbols the price screen renders — £ (U+00A3), ¥ (U+00A5),
€ (U+20AC), ₿ (U+20BF). `$` (U+0024) is already in ASCII.

Hinting is dropped (`--no-hinting`) because the renderer rasterises at
fixed pixel sizes well above where TrueType hinting helps; this trims
~30% off the resulting subset.

Licensed under SIL Open Font License 1.1 — compatible with the project's
Apache-2.0 firmware licence. See upstream `LICENSE.txt` in the Inter 4.1
distribution.

## OpenRunde / OpenRundeBold

OpenRunde is sourced from the upstream
`lauridskern/open-runde` repository (desktop OTFs under
`src/desktop/`), subsetted to BTClock's common glyph range
(printable ASCII + £/¥/€/₿). BTClock uses **SemiBold as the family
base cut** for `OpenRunde.ttf`, while `OpenRundeBold.ttf` remains the
markdown bold role cut. The family is exposed in firmware as
`fontName: "openRunde"`.

```sh
curl -L -o /tmp/OpenRunde-Semibold.otf \
    "https://raw.githubusercontent.com/lauridskern/open-runde/main/src/desktop/OpenRunde-Semibold.otf"
curl -L -o /tmp/OpenRunde-Bold.otf \
    "https://raw.githubusercontent.com/lauridskern/open-runde/main/src/desktop/OpenRunde-Bold.otf"
pyftsubset /tmp/OpenRunde-Semibold.otf \
    --output-file=OpenRunde.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/OpenRunde-Bold.otf \
    --output-file=OpenRundeBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1 (see upstream `LICENSE.txt`).

## Roboto / RobotoBold

Roboto (Google Fonts) via the upstream variable font in `ofl/roboto`.
BTClock uses **SemiBold as the family base cut** for `Roboto.ttf`
(chosen by A/B render comparison for stronger panel legibility), with
`RobotoBold.ttf` as the markdown bold role cut.

```sh
curl -L -o /tmp/Roboto-VF.ttf \
    "https://github.com/google/fonts/raw/main/ofl/roboto/Roboto%5Bwdth,wght%5D.ttf"
python3 -m fontTools.varLib.instancer /tmp/Roboto-VF.ttf wght=600 \
    -o /tmp/Roboto-SemiBold.ttf
python3 -m fontTools.varLib.instancer /tmp/Roboto-VF.ttf wght=700 \
    -o /tmp/Roboto-Bold.ttf

pyftsubset /tmp/Roboto-SemiBold.ttf \
    --output-file=Roboto.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Roboto-Bold.ttf \
    --output-file=RobotoBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under Apache-2.0 via Google Fonts.

## NotoSans / NotoSansBold

Noto Sans (Google Fonts) via the upstream variable font in
`ofl/notosans`. BTClock uses **SemiBold as the family base cut** for
`NotoSans.ttf`, with `NotoSansBold.ttf` for markdown bold.

```sh
curl -L -o /tmp/NotoSans-VF.ttf \
    "https://github.com/google/fonts/raw/main/ofl/notosans/NotoSans%5Bwdth,wght%5D.ttf"
python3 -m fontTools.varLib.instancer /tmp/NotoSans-VF.ttf wght=600 \
    -o /tmp/NotoSans-SemiBold.ttf
python3 -m fontTools.varLib.instancer /tmp/NotoSans-VF.ttf wght=700 \
    -o /tmp/NotoSans-Bold.ttf

pyftsubset /tmp/NotoSans-SemiBold.ttf \
    --output-file=NotoSans.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/NotoSans-Bold.ttf \
    --output-file=NotoSansBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1.

## Ubuntu / UbuntuBold

Ubuntu (Canonical / Dalton Maag), sourced from Google Fonts static TTFs.
The classic Ubuntu family does not ship a `SemiBold`; BTClock's base cut
uses **Ubuntu Medium** as the nearest equivalent for the Semibold-vs-
Regular legibility decision, while `UbuntuBold.ttf` is the markdown bold
role cut.

```sh
curl -L -o /tmp/Ubuntu-Medium.ttf \
    "https://github.com/google/fonts/raw/main/ufl/ubuntu/Ubuntu-Medium.ttf"
curl -L -o /tmp/Ubuntu-Bold.ttf \
    "https://github.com/google/fonts/raw/main/ufl/ubuntu/Ubuntu-Bold.ttf"

pyftsubset /tmp/Ubuntu-Medium.ttf \
    --output-file=Ubuntu.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Ubuntu-Bold.ttf \
    --output-file=UbuntuBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under Ubuntu Font Licence 1.0.

## Azeret

Azeret Mono (Displaay Type Foundry, designed by Hugo Dumont). A
contemporary monospace typeface — the only mono option in the
catalogue. Sharp, geometric counters and uniform advance widths read
cleanly alongside the price-screen separator digits at panel sizes.

The catalogue ships **Regular (wght=400)** only (`Azeret.ttf`, picker
id `azeret`). The previously shipped SemiBold (wght=600) cut was
retired to recover Rev A flash headroom — at ~6 KB gzipped it pushed
the 4 MB Rev A app partition uncomfortably close to full. Azeret
Mono's Regular survives the 1-bpp e-paper threshold cleanly (verified
via WASM A/B render + live Rev B inspection), so the family ships as
single-weight and `Bundle(kAzeret).bold` doubles the Regular like
base Antonio does for its lone cut.

```sh
curl -L -o /tmp/AzeretMono-Regular.ttf \
    "https://github.com/displaay/Azeret/raw/main/fonts/ttf/AzeretMono-Regular.ttf"

pyftsubset /tmp/AzeretMono-Regular.ttf \
    --output-file=Azeret.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --ignore-missing-unicodes \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
```

Licensed under SIL Open Font License 1.1.

## SourceSerif / SourceSerifBold

Source Serif 4 (Adobe). A modern transitional serif designed for
on-screen reading at body sizes. BTClock uses **Semibold as the family
base cut** (`SourceSerif.ttf`) for stronger e-paper readability, with
`SourceSerifBold.ttf` as the markdown bold role cut.

Sourced from the upstream `adobe-fonts/source-serif` release TTF payload:

```sh
curl -L -o /tmp/SourceSerif4-Semibold.ttf \
    "https://github.com/adobe-fonts/source-serif/raw/release/TTF/SourceSerif4-Semibold.ttf"
curl -L -o /tmp/SourceSerif4-Bold.ttf \
    "https://github.com/adobe-fonts/source-serif/raw/release/TTF/SourceSerif4-Bold.ttf"

pyftsubset /tmp/SourceSerif4-Semibold.ttf \
    --output-file=SourceSerif.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/SourceSerif4-Bold.ttf \
    --output-file=SourceSerifBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
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
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Merriweather-Bold.ttf \
    --output-file=MerriweatherBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
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
(`fonts/variable/Bitter[wght].ttf`), so we instance it at wght=600
(base cut) and wght=700 (bold role) with `fonttools varLib.instancer`
before subsetting. This
matches the static-font output Google Fonts publishes downstream while
keeping the upstream provenance traceable.

```sh
curl -L -o /tmp/Bitter-VF.ttf \
    "https://github.com/solmatas/BitterPro/raw/master/fonts/variable/Bitter%5Bwght%5D.ttf"

python3 -m fontTools.varLib.instancer /tmp/Bitter-VF.ttf wght=600 \
    -o /tmp/Bitter-SemiBold.ttf
python3 -m fontTools.varLib.instancer /tmp/Bitter-VF.ttf wght=700 \
    -o /tmp/Bitter-Bold.ttf

pyftsubset /tmp/Bitter-SemiBold.ttf \
    --output-file=Bitter.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/Bitter-Bold.ttf \
    --output-file=BitterBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
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
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
    --drop-tables+=GPOS,GSUB,DSIG \
    --layout-features='*' \
    --no-hinting
pyftsubset /tmp/AtkinsonHyperlegible-Bold.ttf \
    --output-file=AtkinsonBold.ttf \
    --unicodes=U+0020-007E,U+00A3,U+00A5,U+20AC,U+20BF \
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
| OpenRunde             |  11.3 KB |
| OpenRundeBold         |  11.6 KB |
| SourceSerif           |  11.5 KB |
| SourceSerifBold       |  11.4 KB |
| Merriweather          |  11.8 KB |
| MerriweatherBold      |  11.6 KB |
| Bitter                |  11.0 KB |
| BitterBold            |  11.0 KB |
| Atkinson              |   9.0 KB |
| AtkinsonBold          |   8.8 KB |
| Roboto                |  17.5 KB |
| RobotoBold            |  17.6 KB |
| NotoSans              |  16.7 KB |
| NotoSansBold          |  16.8 KB |
| Ubuntu                |   9.4 KB |
| UbuntuBold            |   9.3 KB |
| Azeret                |   8.7 KB |
| SatoshiSymbol         |   3.5 KB |
| MaterialDesignIcons   |   1.0 KB |

Subset extents — every selectable family (Antonio, Oswald, Inter,
SourceSerif, Merriweather, Bitter, Atkinson, OpenRunde, Roboto,
NotoSans, Ubuntu, Azeret) carries printable ASCII (U+0020..U+007E) plus
£/¥/€/₿ (U+00A3, U+00A5, U+20AC, U+20BF) so the price screen's currency
symbol set works regardless of the chosen `fontName`. ₿ is requested
unconditionally; upstreams without the
glyph silently drop it (the verify pass in `tools/fonts/regen.sh`
prints which families ended up with it).

The `SatoshiSymbol_source.woff2` archive (2.1 KB) is kept alongside
the TTF for provenance and is not compiled into firmware.
