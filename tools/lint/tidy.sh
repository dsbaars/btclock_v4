#!/usr/bin/env bash
# Run clang-tidy over the host-test sources. Targets the host build
# because it produces a `compile_commands.json` cmake exports out of
# the box — the IDF firmware build also has one under build-*/ but its
# include graph (mbedTLS, lwIP, FreeRTOS internals) drowns the report
# in noise. Run firmware-side checks locally with:
#
#   /opt/homebrew/opt/llvm/bin/clang-tidy -p build-rev-b path/to/file.cpp
#
# Configure base in `.clang-tidy`.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
CLANG_CXX=""
# Brew on Apple Silicon ships clang-tidy under llvm/bin/, NOT in the
# default PATH. Probe the canonical location before failing. We also
# need the matching clang++ — Apple's xcode clang headers ship under
# the SDK path that LLVM-from-brew can't find unless cmake is told to
# use brew's clang++ as the compiler. Linux CI is the easy case: apt's
# clang-tidy + gcc agree on include paths out of the box.
if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]]; then
    CLANG_TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy
    CLANG_CXX=/opt/homebrew/opt/llvm/bin/clang++
  else
    echo "error: clang-tidy not found" >&2
    echo "       brew install llvm  (mac) or apt-get install clang-tidy (linux)" >&2
    exit 127
  fi
fi

# Need compile_commands.json from the host build. If the existing build
# was configured with a different compiler than our clang-tidy expects,
# wipe + reconfigure so include paths line up. Skip the wipe in CI where
# CLANG_CXX stays empty and the system compiler matches clang-tidy.
NEED_RECONFIGURE=0
if [[ ! -f build-host/compile_commands.json ]]; then
  NEED_RECONFIGURE=1
elif [[ -n "$CLANG_CXX" ]] && ! grep -q "$CLANG_CXX" build-host/compile_commands.json 2>/dev/null; then
  NEED_RECONFIGURE=1
fi
if [[ $NEED_RECONFIGURE -eq 1 ]]; then
  echo "configuring host build with clang-tidy-matching compiler..."
  rm -rf build-host
  if [[ -n "$CLANG_CXX" ]]; then
    CXX="$CLANG_CXX" cmake -S test_host -B build-host \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
  else
    cmake -S test_host -B build-host \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
  fi
fi

# macOS bash 3.2 lacks `mapfile`; xargs keeps this portable. NUL-delimit
# in case any path ever contains spaces.
find test_host -name '*.cpp' -not -path '*/vendor/*' -print0 \
  | xargs -0 "$CLANG_TIDY" -p build-host --quiet "$@"
