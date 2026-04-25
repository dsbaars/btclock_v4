#!/usr/bin/env bash
# Generate one .bin per CSV variant under tools/nvs_gen/variants/, using
# ESP-IDF's nvs_partition_gen. Output goes to tools/nvs_gen/out/ by
# default; pass an explicit --outdir to redirect (e.g. straight into
# web-flasher-ng/public/firmware_v4/nvs/).
#
# Partition size 0x5000 (20 KiB) matches every variant's partitions_*.csv
# nvs entry — the same partition layout for Rev A / Rev B / V8 means one
# set of pre-seed images works on all three boards.
#
# Requires the IDF env to be sourced for nvs_partition_gen.py:
#   source $IDF_PATH/export.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VARIANTS_DIR="${SCRIPT_DIR}/variants"
OUTDIR="${SCRIPT_DIR}/out"
NVS_SIZE="0x5000"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --size)   NVS_SIZE="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Usage: $0 [--outdir <dir>] [--size <bytes>]
  --outdir  Where to write the generated .bin files (default: ${OUTDIR})
  --size    NVS partition size, must match partitions_*.csv (default: 0x5000)
EOF
      exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH not set — run: source \$HOME/esp/v5.5.4/esp-idf/export.sh" >&2
  exit 1
fi

GEN="${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
if [[ ! -f "${GEN}" ]]; then
  echo "nvs_partition_gen.py not found at ${GEN}" >&2
  exit 1
fi

mkdir -p "${OUTDIR}"

shopt -s nullglob
csvs=("${VARIANTS_DIR}"/*.csv)
if [[ ${#csvs[@]} -eq 0 ]]; then
  echo "No CSV variants found in ${VARIANTS_DIR}" >&2
  exit 1
fi

for csv in "${csvs[@]}"; do
  name="$(basename "${csv}" .csv)"
  out="${OUTDIR}/${name}.bin"
  echo "==> ${name}.bin (${NVS_SIZE})"
  python3 "${GEN}" generate "${csv}" "${out}" "${NVS_SIZE}"
done

echo "Done. Output: ${OUTDIR}"
