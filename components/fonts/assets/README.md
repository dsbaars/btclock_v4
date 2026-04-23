# Embedded TTFs

## Antonio / Oswald

Subsetted with kerning/ligature tables stripped. Antonio additionally
includes the common currency symbols used by the price screen (£, ¥, €);
Oswald is ASCII-only since it only renders the split-text label.

To regenerate from upstream Google Fonts sources:

```sh
curl -L -o /tmp/Antonio.ttf "https://github.com/google/fonts/raw/main/ofl/antonio/Antonio%5Bwght%5D.ttf"
curl -L -o /tmp/Oswald.ttf  "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf"

pyftsubset /tmp/Antonio.ttf \
    --unicodes="U+0020-007E,U+00A3,U+00A5,U+20AC" \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Antonio.ttf
pyftsubset /tmp/Oswald.ttf \
    --unicodes=U+0020-007E \
    --drop-tables+=GPOS,GSUB,DSIG --output-file=Oswald.ttf
```

Antonio codepoints: printable ASCII + £ (U+00A3) + ¥ (U+00A5) + € (U+20AC).
`$` (U+0024) is already part of ASCII. Add more symbols here as more
currencies are wired through to the price screen.

Both licensed OFL (SIL Open Font License), compatible with the project's
Apache-2.0 firmware licence.

## DejaVu / DejaVuBold

Used for small label text and currency symbols where Antonio's
condensed proportions read poorly at tiny point sizes. DejaVu Sans is
a humanist sans with generous x-height and distinctive currency glyphs.

Sourced from the upstream DejaVu Fonts 2.37 release:

```sh
curl -L -O "https://downloads.sourceforge.net/project/dejavu/dejavu/2.37/dejavu-fonts-ttf-2.37.tar.bz2"
tar -xf dejavu-fonts-ttf-2.37.tar.bz2
pyftsubset dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf \
    --unicodes=U+0020-007E --drop-tables+=GPOS,GSUB,DSIG --output-file=DejaVu.ttf
pyftsubset dejavu-fonts-ttf-2.37/ttf/DejaVuSans-Bold.ttf \
    --unicodes=U+0020-007E --drop-tables+=GPOS,GSUB,DSIG --output-file=DejaVuBold.ttf
```

Subset range: printable ASCII (U+0020..U+007E), 95 glyphs kept — same
scope as Antonio/Oswald.

Licensed under the DejaVu Fonts License (MIT-style permissive;
Bitstream Vera base + public-domain DejaVu modifications).

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

# then apply the U+E007 padding patch (see commit history /
# beads btclock_v3_fci-891 for the fontTools script)
```

License: see upstream Font Awesome 5.15.4 kit — icon-origin
copyrights are indeterminate per the embedded name-table notice.
The `SatoshiSymbol_source.woff2` is retained in-tree so downstream
licence review can trace the artifact.

## Sizes

In-tree byte counts (`wc -c`), rounded:

| Font           | Size    | Glyphs kept |
|----------------|--------:|------------:|
| Antonio        |  22.3 KB |          98 |
| Oswald         |   8.9 KB |          95 |
| OswaldBold     |   8.9 KB |          95 |
| DejaVu         |  21.2 KB |          95 |
| DejaVuBold     |  19.2 KB |          95 |
| SatoshiSymbol  |   3.5 KB |          16 |

The `SatoshiSymbol_source.woff2` archive (2.1 KB) is kept alongside
the TTF for provenance and is not compiled into firmware.
