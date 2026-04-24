#!/usr/bin/env bash
# Build and OTA-flash the V8 firmware. Override the target with
# DEVICE_HOST=<ip-or-hostname> or credentials with DEVICE_AUTH=user:pass.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/_common.sh"

build_and_flash V8 build-v8 btclock-3ed39c.local 192.168.21.114
