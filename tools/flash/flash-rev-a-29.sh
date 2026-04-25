#!/usr/bin/env bash
# Build and OTA-flash the Rev A 2.9" firmware. Same hardware as Rev A
# (Lolin S3 Mini, 4 MB flash, no BH1750 / no frontlight) — only the EPD
# panel geometry differs (128x296 instead of 122x250). The non-default
# panel is selected via the orthogonal BTCLOCK_PANEL=2_9 build flag.
# Override the target with DEVICE_HOST=<ip-or-hostname> or credentials
# with DEVICE_AUTH=user:pass.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/_common.sh"

# No fallback IP today — set DEVICE_HOST=<ip> until a 2.9" board is
# enumerated on the local LAN.
build_and_flash REV_A build-rev-a-29 btclock-29.local "" 2_9
