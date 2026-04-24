#!/usr/bin/env bash
# Build and OTA-flash the Rev A firmware. Override the target with
# DEVICE_HOST=<ip-or-hostname> or credentials with DEVICE_AUTH=user:pass.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/_common.sh"

build_and_flash REV_A build-rev-a btclock-d600fc.local 192.168.23.125
