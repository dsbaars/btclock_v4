#!/usr/bin/env bash
# Apply or clear a toxic on one relay's toxiproxy proxy.
#
# Usage:
#   ./apply-toxic.sh <relay> <toxic> [arg]
#   ./apply-toxic.sh <relay> clear
#
# <relay>: primal | noslol | dbtc
# <toxic>:
#   timeout   <ms>   — close the connection after <ms> ms (mid-stream).
#                       Reproduces "relay went dark while WSS was open" —
#                       the failure mode our serial logs showed.
#   reset_peer <ms>  — TCP RST after <ms> ms (vs clean FIN). Some
#                       error paths in mbedtls handle RST differently.
#   slow_close <ms>  — delay close by <ms> ms with no data. Useful for
#                       stressing the device's read-timeout handling.
#   latency <ms>     — add <ms> ms one-way. Doesn't kill the relay,
#                       just makes it slow.
#   bandwidth <kBps> — cap throughput.
#   clear            — remove all toxics, restore healthy upstream.
#
# Examples:
#   ./apply-toxic.sh primal timeout 5000        # close every TCP after 5s
#   ./apply-toxic.sh dbtc   reset_peer 30000    # RST 30s into stream
#   ./apply-toxic.sh noslol latency 2000        # 2s extra one-way
#   ./apply-toxic.sh primal clear               # back to healthy
set -euo pipefail

ADMIN="${ADMIN:-http://127.0.0.1:8474}"
RELAY="${1:?usage: apply-toxic.sh <relay> <toxic> [arg]}"
TOXIC="${2:?usage: apply-toxic.sh <relay> <toxic> [arg]}"
ARG="${3:-}"

case "$RELAY" in
  primal|noslol|dbtc) ;;
  *) echo "unknown relay '$RELAY' (expected primal|noslol|dbtc)" >&2; exit 2 ;;
esac

if [[ "$TOXIC" == "clear" ]]; then
  # List existing toxics and DELETE each one. Idempotent.
  toxics=$(curl -sf "${ADMIN}/proxies/${RELAY}/toxics" | jq -r '.[].name')
  for t in $toxics; do
    curl -sf -X DELETE "${ADMIN}/proxies/${RELAY}/toxics/${t}" >/dev/null
    echo "removed toxic ${RELAY}/${t}"
  done
  if [[ -z "${toxics}" ]]; then
    echo "no toxics on ${RELAY}"
  fi
  exit 0
fi

# Build the JSON payload per-toxic.
case "$TOXIC" in
  timeout)
    [[ -n "$ARG" ]] || { echo "timeout needs <ms>" >&2; exit 2; }
    body=$(jq -n --arg t "$TOXIC" --argjson ms "$ARG" \
      '{name:$t, type:$t, stream:"downstream", attributes:{timeout:$ms}}')
    ;;
  reset_peer)
    [[ -n "$ARG" ]] || { echo "reset_peer needs <ms>" >&2; exit 2; }
    body=$(jq -n --arg t "$TOXIC" --argjson ms "$ARG" \
      '{name:$t, type:$t, stream:"downstream", attributes:{timeout:$ms}}')
    ;;
  slow_close)
    [[ -n "$ARG" ]] || { echo "slow_close needs <ms>" >&2; exit 2; }
    body=$(jq -n --arg t "$TOXIC" --argjson ms "$ARG" \
      '{name:$t, type:$t, stream:"downstream", attributes:{delay:$ms}}')
    ;;
  latency)
    [[ -n "$ARG" ]] || { echo "latency needs <ms>" >&2; exit 2; }
    body=$(jq -n --arg t "$TOXIC" --argjson ms "$ARG" \
      '{name:$t, type:$t, stream:"downstream", attributes:{latency:$ms}}')
    ;;
  bandwidth)
    [[ -n "$ARG" ]] || { echo "bandwidth needs <kBps>" >&2; exit 2; }
    body=$(jq -n --arg t "$TOXIC" --argjson rate "$ARG" \
      '{name:$t, type:$t, stream:"downstream", attributes:{rate:$rate}}')
    ;;
  *)
    echo "unknown toxic '$TOXIC'" >&2
    exit 2
    ;;
esac

# Replace any existing toxic of the same name (toxiproxy 409s on dup).
curl -sf -X DELETE "${ADMIN}/proxies/${RELAY}/toxics/${TOXIC}" >/dev/null 2>&1 || true
curl -sf -X POST -H 'Content-Type: application/json' -d "$body" \
     "${ADMIN}/proxies/${RELAY}/toxics" | jq .
