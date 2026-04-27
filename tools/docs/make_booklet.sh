#!/usr/bin/env bash
# Render the docs as a print-ready PDF booklet.
#
# Outputs:
#   docs/build/btclock-booklet.pdf        — single-page A5 PDF (final read)
#   docs/build/btclock-booklet.tex        — intermediate LaTeX (kept for diffing)
#   docs/build/btclock-booklet-impose.pdf — A4-imposed, fold-and-staple booklet
#                                           (only when `pdfjam` is installed)
#
# Requires (mac/brew):
#   brew install pandoc                 # markdown → tex/pdf
#   brew install --cask mactex-no-gui   # provides xelatex
#   brew install --cask font-inter      # body font (or use any installed family)
#   brew install pdfjam   (optional)    # for the A4-imposed booklet
#
# Usage:
#   tools/docs/make_booklet.sh
#   tools/docs/make_booklet.sh nl       # build the Dutch quickstart edition
#   tools/docs/make_booklet.sh es       # Spanish, de = German
#
# Per-language editions only swap the QUICKSTART for the localised
# variant; the rest of the docs are English (no translations exist for
# HANDBOOK / SETTINGS / etc. yet — the mkdocs-i18n plugin falls back
# to English on the live site too, so the booklet matches what readers
# see online).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

LANG_TAG="${1:-en}"
case "$LANG_TAG" in
  en) QUICKSTART="docs/QUICKSTART.md"        ; LANG_FULL="English"    ;;
  nl) QUICKSTART="docs/QUICKSTART.nl.md"     ; LANG_FULL="Nederlands" ;;
  de) QUICKSTART="docs/QUICKSTART.de.md"     ; LANG_FULL="Deutsch"    ;;
  es) QUICKSTART="docs/QUICKSTART.es.md"     ; LANG_FULL="Español"    ;;
  *)  echo "unknown language tag: $LANG_TAG (want one of: en nl de es)" >&2; exit 2 ;;
esac

OUT_DIR="docs/build"
SUFFIX=""
[[ "$LANG_TAG" != "en" ]] && SUFFIX="-$LANG_TAG"
OUT_PDF="$OUT_DIR/btclock-booklet${SUFFIX}.pdf"
OUT_TEX="$OUT_DIR/btclock-booklet${SUFFIX}.tex"
mkdir -p "$OUT_DIR"

# Order matches the mkdocs.yml nav. The home page comes first so the
# booklet opens with the same landing copy the website does.
DOC_ORDER=(
  docs/index.md
  "$QUICKSTART"
  docs/HANDBOOK.md
  docs/SETTINGS.md
  docs/ARCHITECTURE.md
  docs/BUILD_FROM_SOURCE.md
  docs/WEBUI_MINING_POOL_FIELDS.md
  docs/STORY.md
)

echo "[booklet] language: $LANG_FULL ($LANG_TAG)"
echo "[booklet] inputs: ${#DOC_ORDER[@]} markdown files"

# Pandoc reads gfm-flavoured markdown (the dialect mkdocs-material uses
# for tables / fenced code / task lists), normalises to LaTeX, and runs
# xelatex twice (once for content, once so the TOC settles). Image
# paths inside the markdown are relative to the docs/ tree, so
# --resource-path points there.
pandoc \
  --from gfm+yaml_metadata_block \
  --pdf-engine=xelatex \
  --resource-path "docs:docs/img:." \
  --standalone \
  --metadata-file=tools/docs/booklet.yaml \
  --metadata "lang=$LANG_TAG" \
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
  --resource-path "docs:docs/img:." \
  --standalone \
  --metadata-file=tools/docs/booklet.yaml \
  --metadata "lang=$LANG_TAG" \
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
# installed. Produces an A4-landscape PDF where each sheet carries 4
# A5 pages in print order; print double-sided, fold in half, staple.
if command -v pdfjam >/dev/null 2>&1; then
  IMPOSED="$OUT_DIR/btclock-booklet${SUFFIX}-impose.pdf"
  pdfjam --booklet true --landscape --paper a4paper \
    --outfile "$IMPOSED" "$OUT_PDF" >/dev/null
  echo "[booklet] wrote $IMPOSED (A4 imposed, fold + staple)"
else
  echo "[booklet] (skipping A4 imposition — install pdfjam for fold-and-staple output)"
fi
