# Embedded TTFs

## Antonio / Oswald

Subsetted to printable ASCII (U+0020..U+007E) with kerning/ligature
tables stripped, since the BTClock only ever renders Western digits,
punctuation and a handful of uppercase letters for currency codes.

To regenerate from upstream Google Fonts sources:

```sh
curl -L -o /tmp/Antonio.ttf "https://github.com/google/fonts/raw/main/ofl/antonio/Antonio%5Bwght%5D.ttf"
curl -L -o /tmp/Oswald.ttf  "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf"

pyftsubset /tmp/Antonio.ttf --unicodes=U+0020-007E --drop-tables+=GPOS,GSUB,DSIG --output-file=Antonio.ttf
pyftsubset /tmp/Oswald.ttf  --unicodes=U+0020-007E --drop-tables+=GPOS,GSUB,DSIG --output-file=Oswald.ttf
```

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

### U+E007 padding tweak

The source U+E007 glyph ships with **zero side-bearings**
(advance=324 em, bbox xMin=0..xMax=323 on upem=512), so the ink
fills the advance box and visually kisses the neighbouring digit.
Antonio digits carry ~11 % sidebearings (advance=854, bbox 91..762
on upem=2048), so the sats glyph was widened to match:

- every contour x coordinate shifted **+48 em**
- advance width set to **420 em**
- resulting lsb=48, rsb=49 — symmetric ~11 % margins

Applied via fontTools; only U+E007 is touched, the other 15 PUA
variants are byte-identical.

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
| Antonio        |  21.3 KB |          95 |
| Oswald         |   8.9 KB |          95 |
| OswaldBold     |   8.9 KB |          95 |
| DejaVu         |  21.2 KB |          95 |
| DejaVuBold     |  19.2 KB |          95 |
| SatoshiSymbol  |   3.5 KB |          16 |

The `SatoshiSymbol_source.woff2` archive (2.1 KB) is kept alongside
the TTF for provenance and is not compiled into firmware.
