#!/usr/bin/env bash
# Apply project-local patches to the ESP-IDF tree.
#
# Each patch in this directory is a unified diff (against the IDF root)
# that fixes an upstream bug we hit in production but isn't yet in a
# released IDF. The catalog and removal-criteria live in README.md.
#
# Idempotent: a patch already applied is detected via `git apply
# --reverse --check` and skipped, so this script is safe to run on
# every CI invocation and after every local IDF refresh.
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/v6.0/esp-idf}"
if [ ! -d "$IDF_PATH" ]; then
  echo "IDF_PATH not found: $IDF_PATH" >&2
  exit 1
fi

DIR="$(cd "$(dirname "$0")" && pwd)"

shopt -s nullglob
patches=("$DIR"/*.patch)
if [ "${#patches[@]}" -eq 0 ]; then
  echo "No patches in $DIR — nothing to do."
  exit 0
fi

echo "Applying ${#patches[@]} patch(es) to IDF tree at $IDF_PATH"
for p in "${patches[@]}"; do
  base="$(basename "$p")"
  if git -C "$IDF_PATH" apply --reverse --check "$p" 2>/dev/null; then
    echo "  [skip]  $base (already applied)"
    continue
  fi
  if git -C "$IDF_PATH" apply --check "$p" 2>/dev/null; then
    git -C "$IDF_PATH" apply "$p"
    echo "  [apply] $base"
  else
    echo "  [FAIL]  $base — does not apply cleanly to $IDF_PATH" >&2
    exit 1
  fi
done
