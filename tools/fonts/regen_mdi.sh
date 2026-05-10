#!/usr/bin/env bash
# Regenerate components/fonts/assets/MaterialDesignIcons.ttf with just
# the icons the firmware currently paints. Keeps the subsetted TTF tiny
# — each added icon adds ~500 B, so we keep the list minimal and add
# entries only when a new screen actually needs them.
#
# Run from anywhere. Requires python3, fonttools (`pip install fonttools`).
#
# To add a new icon:
#   1. Find the mdi-<name> class in
#      https://pictogrammers.com/library/mdi/
#   2. Add the name to the list below.
#   3. Rerun this script. The codepoint constant lands in
#      components/fonts/include/mdi_codepoints.hpp automatically.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# Icons the firmware renders. Keep sorted to minimise diff noise.
# Also extend components/fonts/mdi_custom_cell.cpp `kSubset` so POST
# /api/show/custom cells `mdi:<name>` resolve for every listed icon.
ICONS=(
    alarm
    arrow-down-bold
    arrow-up-bold
    bell
    bitcoin
    clock-outline
    lightning-bolt
    pickaxe
    rocket-launch
    wifi
    wifi-alert
    wifi-off
)

# MDI git ref. Pin to a tag when a stable release is needed; `master`
# is fine for active development — reruns produce identical output as
# long as upstream hasn't changed.
REF="${MDI_REF:-master}"

python3 "${HERE}/subset_mdi.py" --ref "${REF}" "${ICONS[@]}"
