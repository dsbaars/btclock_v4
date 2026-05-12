#!/usr/bin/env bash
# Render N candidate TTFs as block-height samples through the BTClock
# WASM pipeline. Generic counterpart of render_font_ab.sh (which is
# fixed at one baseline + one candidate); this one accepts an arbitrary
# list of `--weight name=path` pairs so you can compare three or more
# weights of a single candidate face in one swoop.
#
# Each candidate TTF is hot-swapped into an existing selectable font
# slot (the one named by `--asset` / `--family`), the WASM bundle is
# rebuilt, then tools/wasm/render_font_sample.mjs writes a PCB-framed
# PNG to docs/img/fonts/<font-id>_<weight>.png. The original asset is
# restored and the WASM module rebuilt once more at the end so the dist
# tree returns to its baseline state — same restore discipline as
# render_font_ab.sh.
#
# The candidate TTFs are expected to be pre-subsetted (BTClock's
# standard ASCII + £/¥/€/₿ range). Subset the upstream files first with
# `pyftsubset` or use a font-specific wrapper script (see
# render_azeret.sh for an example that does the curl + subset before
# delegating here).
#
# Usage:
#   tools/wasm/render_font_candidates.sh \
#     --font-id azeret \
#     --asset components/fonts/assets/Inter.ttf \
#     --family 2 \
#     --weight regular=/tmp/AzeretMono-Regular-subset.ttf \
#     --weight semibold=/tmp/AzeretMono-SemiBold-subset.ttf \
#     --weight bold=/tmp/AzeretMono-Bold-subset.ttf
#
# Outputs (one per --weight pair):
#   docs/img/fonts/azeret_regular.png
#   docs/img/fonts/azeret_semibold.png
#   docs/img/fonts/azeret_bold.png
#
# Convention: --weight names are lower-case and used verbatim in the
# output filename. Re-running overwrites existing PNGs.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
OUT_DIR="${REPO}/docs/img/fonts"
TMPDIR_WASM="${TMPDIR_WASM:-/var/tmp/emscripten-tmp}"

font_id=""
asset=""
family=""
height="897654"
panels="7"
vertical_desc="true"
inverted="false"
weights=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --font-id) font_id="$2"; shift 2 ;;
    --asset) asset="$2"; shift 2 ;;
    --family) family="$2"; shift 2 ;;
    --height) height="$2"; shift 2 ;;
    --panels) panels="$2"; shift 2 ;;
    --vertical-desc) vertical_desc="$2"; shift 2 ;;
    --inverted) inverted="$2"; shift 2 ;;
    --weight) weights+=("$2"); shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${font_id}" || -z "${asset}" || -z "${family}" || ${#weights[@]} -eq 0 ]]; then
  cat >&2 <<USAGE
Usage: $0 \\
  --font-id <id>           output filename prefix, e.g. 'azeret'
  --asset <path>           slot to swap into (e.g. components/fonts/assets/Inter.ttf)
  --family <int>           render_font_sample.mjs family id matching the slot (e.g. 2 for inter)
  --weight <name>=<path>   repeatable; one candidate TTF per weight to render
  [--height <int>]         block height shown in the sample (default 897654)
  [--panels 7|8]           panel count (default 7)
  [--vertical-desc true|false] (default true)
  [--inverted true|false]  (default false)
USAGE
  exit 2
fi

asset_abs="${asset}"
[[ "${asset_abs}" != /* ]] && asset_abs="${REPO}/${asset_abs}"

if [[ ! -f "${asset_abs}" ]]; then
  echo "Asset not found: ${asset_abs}" >&2
  exit 1
fi

# Pre-flight: validate every candidate path before touching the asset
# (so a typo doesn't leave the working tree with a half-swapped Inter).
for pair in "${weights[@]}"; do
  name="${pair%%=*}"
  path="${pair#*=}"
  if [[ "${pair}" != *=* || -z "${name}" || -z "${path}" ]]; then
    echo "Malformed --weight '${pair}' (expected name=path)" >&2
    exit 2
  fi
  if [[ ! -s "${path}" ]]; then
    echo "Candidate TTF not found: ${path}" >&2
    exit 1
  fi
done

if ! command -v em++ >/dev/null 2>&1; then
  echo "em++ not in PATH — install emscripten before running this script." >&2
  exit 127
fi

# render_font_sample.mjs imports sharp from data/node_modules/, so the
# WebUI workspace must have been pnpm-installed at least once.
if [[ ! -f "${REPO}/data/node_modules/sharp/lib/index.js" ]]; then
  echo "data/node_modules/sharp not present — run 'pnpm install' in data/." >&2
  exit 1
fi

backup="$(mktemp -t render_font_candidates.XXXXXX)"
cp "${asset_abs}" "${backup}"
restore() {
  cp "${backup}" "${asset_abs}" || true
  rm -f "${backup}" || true
}
trap restore EXIT

mkdir -p "${OUT_DIR}" "${TMPDIR_WASM}"

for pair in "${weights[@]}"; do
  name="${pair%%=*}"
  path="${pair#*=}"
  out="${OUT_DIR}/${font_id}_${name}.png"
  echo "[swap+build] ${name} -> $(basename "${asset_abs}")"
  cp "${path}" "${asset_abs}"
  TMPDIR="${TMPDIR_WASM}" "${HERE}/build.sh"
  echo "[render] -> ${out}"
  node "${HERE}/render_font_sample.mjs" \
    --family "${family}" \
    --out "${out}" \
    --height "${height}" \
    --panels "${panels}" \
    --vertical-desc "${vertical_desc}" \
    --inverted "${inverted}"
done

echo "[restore] $(basename "${asset_abs}")"
cp "${backup}" "${asset_abs}"
TMPDIR="${TMPDIR_WASM}" "${HERE}/build.sh"

echo "[ok] wrote:"
for pair in "${weights[@]}"; do
  name="${pair%%=*}"
  echo "  ${OUT_DIR}/${font_id}_${name}.png"
done
