#!/usr/bin/env bash
# Render the docs as a print-ready PDF booklet (English only).
#
# Outputs:
#   docs/build/btclock-booklet.pdf        — single A4 PDF (final read)
#   docs/build/btclock-booklet.tex        — intermediate LaTeX (kept for diffing)
#   docs/build/btclock-booklet-impose.pdf — A3 imposed, fold-and-staple booklet
#                                           (only when `pdfjam` is installed)
#
# Requires (mac/brew):
#   brew install pandoc                 # markdown → tex/pdf
#   brew install --cask mactex-no-gui   # provides xelatex
#   brew install --cask font-inter      # body font (or use any installed family)
#   brew install pdfjam   (optional)    # for the A3-imposed booklet
#   node + npm                          # for `npx mmdc` mermaid pre-render
#
# Only English is built. Quickstart translations exist on the live
# mkdocs site, but HANDBOOK / SETTINGS / ARCHITECTURE / etc. are
# English-only — a localised booklet would be ~95% English with one
# translated chapter, which is more confusing than helpful.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

OUT_DIR="docs/build"
OUT_PDF="$OUT_DIR/btclock-booklet.pdf"
OUT_TEX="$OUT_DIR/btclock-booklet.tex"
mkdir -p "$OUT_DIR"

# Order matches the mkdocs.yml nav. The home page comes first so the
# booklet opens with the same landing copy the website does.
DOC_ORDER=(
  docs/index.md
  docs/QUICKSTART.md
  docs/HANDBOOK.md
  docs/SETTINGS.md
  docs/ARCHITECTURE.md
  docs/BUILD_FROM_SOURCE.md
  docs/WEBUI_MINING_POOL_FIELDS.md
  docs/STORY.md
)

echo "[booklet] inputs: ${#DOC_ORDER[@]} markdown files"

# Pre-render ```mermaid fences to PNGs so xelatex can include them.
# MkDocs renders mermaid client-side via Material's bundled JS; pandoc
# can't, so we shell out to mermaid-cli (`mmdc`) and substitute an
# ![](png) reference into a preprocessed markdown copy. Use a globally
# installed mmdc when available, otherwise fall back to npx.
PRE_DIR="$OUT_DIR/booklet-src"
DIAG_DIR="$PRE_DIR/diagrams"
mkdir -p "$PRE_DIR" "$DIAG_DIR"

if command -v mmdc >/dev/null 2>&1; then
  MMDC_CMD="mmdc"
elif command -v npx >/dev/null 2>&1; then
  MMDC_CMD="npx -y --package=@mermaid-js/mermaid-cli mmdc"
else
  MMDC_CMD=""
  echo "[booklet] (no mmdc / npx available — mermaid blocks will render as code)"
fi

PROCESSED_ORDER=()
for src in "${DOC_ORDER[@]}"; do
  if grep -q '^```mermaid' "$src" 2>/dev/null && [ -n "$MMDC_CMD" ]; then
    base="$(basename "$src" .md)"
    dst="$PRE_DIR/$(basename "$src")"
    python3 tools/docs/render_mermaid.py \
      "$src" "$dst" "$DIAG_DIR" "$base" "$MMDC_CMD"
    PROCESSED_ORDER+=( "$dst" )
  else
    PROCESSED_ORDER+=( "$src" )
  fi
done
DOC_ORDER=( "${PROCESSED_ORDER[@]}" )

# Pandoc reads gfm-flavoured markdown (the dialect mkdocs-material uses
# for tables / fenced code / task lists), normalises to LaTeX, and runs
# xelatex twice (once for content, once so the TOC settles). Image
# paths inside the markdown are relative to the docs/ tree, so
# --resource-path points there.
pandoc \
  --from gfm+yaml_metadata_block \
  --pdf-engine=xelatex \
  --resource-path "docs:docs/img:$PRE_DIR:." \
  --standalone \
  --metadata-file=tools/docs/booklet.yaml \
  --lua-filter=tools/docs/equal_table_widths.lua \
  --include-in-header=tools/docs/booklet-header.tex \
  --toc --toc-depth=2 \
  --top-level-division=chapter \
  --shift-heading-level-by=0 \
  -V documentclass=report \
  -V classoption:openright \
  -V colorlinks=true \
  -V linkcolor=BTOrange \
  -V urlcolor=BTOrange \
  -V toccolor=BTOrange \
  -o "$OUT_PDF" \
  "${DOC_ORDER[@]}"

# Also emit the intermediate .tex so the result can be diffed across
# runs (the .pdf binary diff isn't useful).
pandoc \
  --from gfm+yaml_metadata_block \
  --to latex \
  --resource-path "docs:docs/img:$PRE_DIR:." \
  --standalone \
  --metadata-file=tools/docs/booklet.yaml \
  --lua-filter=tools/docs/equal_table_widths.lua \
  --include-in-header=tools/docs/booklet-header.tex \
  --toc --toc-depth=2 \
  --top-level-division=chapter \
  -V documentclass=report \
  -V classoption:openright \
  -V colorlinks=true \
  -V linkcolor=BTOrange \
  -V urlcolor=BTOrange \
  -V toccolor=BTOrange \
  -o "$OUT_TEX" \
  "${DOC_ORDER[@]}"

bytes=$(stat -f%z "$OUT_PDF" 2>/dev/null || stat -c%s "$OUT_PDF")
echo "[booklet] wrote $OUT_PDF ($((bytes / 1024)) KB)"
echo "[booklet] wrote $OUT_TEX (intermediate LaTeX)"

# Imposed (fold-and-staple) booklet — optional, only if pdfjam is
# installed. Source pages are A4, so the booklet is imposed onto A3
# landscape: each sheet carries 2 A4 pages per side, print double-
# sided, fold in half once, staple.
if command -v pdfjam >/dev/null 2>&1; then
  IMPOSED="$OUT_DIR/btclock-booklet-impose.pdf"
  pdfjam --booklet true --landscape --paper a3paper \
    --outfile "$IMPOSED" "$OUT_PDF" >/dev/null
  echo "[booklet] wrote $IMPOSED (A3 imposed, fold + staple → A4 booklet)"
else
  echo "[booklet] (skipping imposition — install pdfjam for fold-and-staple output)"
fi
