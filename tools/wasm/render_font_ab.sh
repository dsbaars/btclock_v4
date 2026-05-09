#!/usr/bin/env bash
# Fast A/B renderer for one font base asset.
#
# Usage:
#   tools/wasm/render_font_ab.sh \
#     --family 1 \
#     --font-id oswald \
#     --asset components/fonts/assets/Oswald.ttf \
#     --candidate /tmp/Oswald-SemiBold.ttf
#
# Outputs:
#   docs/img/fonts/<font-id>_regular_candidate.png
#   docs/img/fonts/<font-id>_semibold_candidate.png
#
# Notes:
# - Rebuilds WASM twice (regular + candidate), but only renders one sample
#   image per run (much faster than regenerating all docs screens).
# - Restores the original asset at the end and rebuilds WASM once more.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
TMPDIR_WASM="${TMPDIR_WASM:-/var/tmp/emscripten-tmp}"

family=""
font_id=""
asset=""
candidate=""
height="897654"
panels="7"
vertical_desc="true"
inverted="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --family) family="$2"; shift 2 ;;
    --font-id) font_id="$2"; shift 2 ;;
    --asset) asset="$2"; shift 2 ;;
    --candidate) candidate="$2"; shift 2 ;;
    --height) height="$2"; shift 2 ;;
    --panels) panels="$2"; shift 2 ;;
    --vertical-desc) vertical_desc="$2"; shift 2 ;;
    --inverted) inverted="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$family" || -z "$font_id" || -z "$asset" || -z "$candidate" ]]; then
  echo "Usage: $0 --family <id> --font-id <name> --asset <path> --candidate <path> [--height n] [--panels 7|8] [--vertical-desc true|false] [--inverted true|false]" >&2
  exit 2
fi

asset_abs="${asset}"
candidate_abs="${candidate}"
if [[ "${asset_abs}" != /* ]]; then asset_abs="${REPO}/${asset_abs}"; fi
if [[ "${candidate_abs}" != /* ]]; then candidate_abs="${REPO}/${candidate_abs}"; fi

if [[ ! -f "${asset_abs}" ]]; then
  echo "Asset not found: ${asset_abs}" >&2
  exit 1
fi
if [[ ! -f "${candidate_abs}" ]]; then
  echo "Candidate not found: ${candidate_abs}" >&2
  exit 1
fi

backup="$(mktemp)"
cp "${asset_abs}" "${backup}"

restore() {
  cp "${backup}" "${asset_abs}" || true
  rm -f "${backup}" || true
}
trap restore EXIT

mkdir -p "${REPO}/docs/img/fonts" "${TMPDIR_WASM}"

echo "== regular baseline =="
TMPDIR="${TMPDIR_WASM}" "${REPO}/tools/wasm/build.sh"
node "${REPO}/tools/wasm/render_font_sample.mjs" \
  --family "${family}" \
  --out "${REPO}/docs/img/fonts/${font_id}_regular_candidate.png" \
  --height "${height}" \
  --panels "${panels}" \
  --vertical-desc "${vertical_desc}" \
  --inverted "${inverted}"

echo "== candidate =="
cp "${candidate_abs}" "${asset_abs}"
TMPDIR="${TMPDIR_WASM}" "${REPO}/tools/wasm/build.sh"
node "${REPO}/tools/wasm/render_font_sample.mjs" \
  --family "${family}" \
  --out "${REPO}/docs/img/fonts/${font_id}_semibold_candidate.png" \
  --height "${height}" \
  --panels "${panels}" \
  --vertical-desc "${vertical_desc}" \
  --inverted "${inverted}"

echo "== restore original asset + rebuild =="
cp "${backup}" "${asset_abs}"
TMPDIR="${TMPDIR_WASM}" "${REPO}/tools/wasm/build.sh"

echo "[ok] wrote:"
echo "  docs/img/fonts/${font_id}_regular_candidate.png"
echo "  docs/img/fonts/${font_id}_semibold_candidate.png"
