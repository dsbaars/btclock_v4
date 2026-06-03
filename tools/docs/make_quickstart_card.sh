#!/usr/bin/env bash
# Render docs/QUICKSTART.md as a standalone print-ready A4 PDF — the
# "card" that ships with the device or gets printed on demand. Same
# pandoc/xelatex stack as the full booklet (tools/docs/make_booklet.sh),
# different geometry: tighter margins, article class (no chapter
# numbering), single-sided.
#
# Output:
#   docs/build/btclock-quickstart.pdf
#
# Requires: pandoc, xelatex (mactex / basictex), Inter, DejaVu Sans Mono.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

OUT_DIR="docs/build"
OUT_PDF="$OUT_DIR/btclock-quickstart.pdf"
mkdir -p "$OUT_DIR"

SRC="tools/docs/quickstart-card.md"

# Footer build stamp — UTC, injected as a LaTeX \renewcommand the header
# reads. Done in the build (not LaTeX) because \year/\month/\day are local
# time and don't expose a UTC offset.
STAMP_TEX="$(mktemp)"
trap 'rm -f "$STAMP_TEX"' EXIT
printf '\\renewcommand{\\btBuildStamp}{%s}\n' "$(date -u '+%Y-%m-%d %H:%M UTC')" \
  > "$STAMP_TEX"

pandoc \
  --from markdown+yaml_metadata_block-implicit_figures \
  --pdf-engine=xelatex \
  --resource-path "docs:docs/img:." \
  --standalone \
  --metadata-file=tools/docs/quickstart-card.yaml \
  --include-in-header=tools/docs/quickstart-card-header.tex \
  --include-in-header="$STAMP_TEX" \
  --top-level-division=section \
  -V documentclass=article \
  -V colorlinks=true \
  -V linkcolor=BTOrange \
  -V urlcolor=BTOrange \
  -V toccolor=BTOrange \
  -o "$OUT_PDF" \
  "$SRC"

bytes=$(stat -f%z "$OUT_PDF" 2>/dev/null || stat -c%s "$OUT_PDF")
echo "[quickstart] wrote $OUT_PDF ($((bytes / 1024)) KB)"
