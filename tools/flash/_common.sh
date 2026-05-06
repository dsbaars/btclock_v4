#!/usr/bin/env bash
# Shared helpers for the per-variant flash scripts in this directory.
# Sourced, not executed. Each flash-<variant>.sh picks its BTCLOCK_BOARD,
# build dir, mDNS name, and calls build_and_flash.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Resolve the ESP-IDF export.sh path. Precedence:
#   1. $IDF_EXPORT — explicit override
#   2. $IDF_PATH/export.sh — already-active IDF install
#   3. ~/esp/v6.0/esp-idf/export.sh — project's supported toolchain
#   4. ~/esp/v5.5.4/esp-idf/export.sh — fallback
resolve_idf_export() {
  if [[ -n "${IDF_EXPORT:-}" && -f "${IDF_EXPORT}" ]]; then
    echo "${IDF_EXPORT}"
    return 0
  fi
  if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
    echo "${IDF_PATH}/export.sh"
    return 0
  fi
  local candidate
  for candidate in \
    "${HOME}/esp/v6.0/esp-idf/export.sh" \
    "${HOME}/esp/v5.5.4/esp-idf/export.sh"; do
    if [[ -f "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

# Source the ESP-IDF env into the current shell. Noisy on first run
# because idf.py bootstrap prints a banner; silenced here so the
# build output stays readable.
source_idf() {
  local idf_export
  if ! idf_export="$(resolve_idf_export)"; then
    echo "error: ESP-IDF export.sh not found." >&2
    echo "  set IDF_EXPORT=/path/to/esp-idf/export.sh, or" >&2
    echo "  set IDF_PATH=/path/to/esp-idf, or" >&2
    echo "  install IDF at ~/esp/v6.0/esp-idf or ~/esp/v5.5.4/esp-idf" >&2
    exit 1
  fi
  # export.sh writes to stdout/stderr; swallow but keep failure visible.
  # shellcheck disable=SC1090
  source "${idf_export}" >/dev/null
}

# Build one variant. Args: btclock_board, build_dir, [btclock_panel].
# The panel arg is optional — omit it to use the CMake default (2_13).
# When present, it lands as -DBTCLOCK_PANEL=<panel>, the orthogonal
# panel selector that the EPD driver factory consumes.
build_variant() {
  local btclock_board="$1"
  local build_dir="$2"
  local btclock_panel="${3:-}"
  (
    cd "${REPO_ROOT}"
    local args=(
      -B "${build_dir}"
      -D "BTCLOCK_BOARD=${btclock_board}"
      -D "SDKCONFIG=${build_dir}/sdkconfig"
    )
    if [[ -n "${btclock_panel}" ]]; then
      args+=(-D "BTCLOCK_PANEL=${btclock_panel}")
    fi
    idf.py "${args[@]}" build
  )
}

# Resolve the device HTTP host. Order of precedence:
#   1. $DEVICE_HOST (explicit override, e.g. DEVICE_HOST=192.168.20.97)
#   2. mDNS name (btclock-<mac_suffix>.local)
#   3. fallback: last-known IP embedded in the caller script
# Args: mdns_name, fallback_ip
# Echos the resolved host:port on stdout.
resolve_host() {
  local mdns="$1"
  local fallback_ip="$2"
  if [[ -n "${DEVICE_HOST:-}" ]]; then
    echo "${DEVICE_HOST}"
    return 0
  fi
  # curl with -o/dev/null returns 0 for any HTTP response including 401
  # (auth gate) — we only want to know the device answered on port 80.
  if curl -sS --max-time 5 -o /dev/null "http://${mdns}/api/status" 2>/dev/null; then
    echo "${mdns}"
    return 0
  fi
  if [[ -n "${fallback_ip}" ]] && \
     curl -sS --max-time 5 -o /dev/null "http://${fallback_ip}/api/status" 2>/dev/null; then
    echo "${fallback_ip}"
    return 0
  fi
  echo "error: can't reach ${mdns} or ${fallback_ip}" >&2
  return 1
}

# OTA upload one firmware binary. Args: host, bin_path.
# Honours $DEVICE_AUTH=user:pass for boards with httpAuthEnabled.
ota_flash() {
  local host="$1"
  local bin="$2"
  local auth=()
  if [[ -n "${DEVICE_AUTH:-}" ]]; then
    auth=(-u "${DEVICE_AUTH}")
  fi
  echo "==> uploading ${bin} ($(du -h "${bin}" | awk '{print $1}')) to http://${host}/upload/firmware"
  curl -fSs --max-time 240 ${auth[@]+"${auth[@]}"} \
    -X POST \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@${bin}" \
    "http://${host}/upload/firmware"
  echo
}

# Poll /api/status until the device reports a fresh boot. Args: host.
# Gives up after 60s — at that point something's wrong and the caller
# should inspect the device manually.
wait_for_reboot() {
  local host="$1"
  local auth=()
  if [[ -n "${DEVICE_AUTH:-}" ]]; then
    auth=(-u "${DEVICE_AUTH}")
  fi
  echo "==> waiting for reboot"
  local deadline=$(( SECONDS + 60 ))
  while (( SECONDS < deadline )); do
    local uptime
    uptime="$(curl -sf --max-time 3 ${auth[@]+"${auth[@]}"} "http://${host}/api/status" 2>/dev/null \
      | python3 -c 'import json,sys;print(json.load(sys.stdin).get("espUptime",""))' 2>/dev/null)"
    if [[ -n "${uptime}" && "${uptime}" -lt 120 ]]; then
      echo "==> back up (uptime=${uptime}s)"
      return 0
    fi
    sleep 2
  done
  echo "error: device ${host} didn't respond within 60s" >&2
  return 1
}

# End-to-end: source IDF, build, resolve host, OTA, confirm.
# Args: btclock_board, build_dir, mdns_name, fallback_ip, [btclock_panel].
# Panel arg is optional and only required for non-default geometries
# (e.g. flash-rev-a-29 passes "2_9").
build_and_flash() {
  local btclock_board="$1"
  local build_dir="$2"
  local mdns_name="$3"
  local fallback_ip="$4"
  local btclock_panel="${5:-}"

  source_idf
  build_variant "${btclock_board}" "${build_dir}" "${btclock_panel}"

  local host
  host="$(resolve_host "${mdns_name}" "${fallback_ip}")"
  echo "==> target: ${host}"

  ota_flash "${host}" "${REPO_ROOT}/${build_dir}/btclock_v4.bin"
  wait_for_reboot "${host}"
}
