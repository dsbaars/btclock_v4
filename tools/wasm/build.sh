#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh — compile the IDF-port's screen-layout helpers to WebAssembly.
#
# Sources: idf_cpp_proto/main/screens/common.cpp (+ fee_rate_layout.hpp,
# header-only) and tools/wasm/binding.cpp (embind glue). No ESP-IDF
# headers are pulled in — the binding forward-declares the pure helpers
# from common.cpp so we can compile without the device toolchain.
#
# Result is a JS/WASM pair that exposes parseBlockHeight / parsePriceData /
# parseSatsPerCurrency / parseBlockFees to a browser, letting the HTML
# preview mimic the EPD rendering without a device.
#
# How to run:
#   1. Install emsdk (see https://emscripten.org/docs/getting_started/downloads.html):
#        git clone https://github.com/emscripten-core/emsdk ~/emsdk
#        cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
#   2. Activate it in this shell:       source ~/emsdk/emsdk_env.sh
#   3. Run:                             ./idf_cpp_proto/tools/wasm/build.sh
#   4. Open preview.html in a browser (via a local HTTP server — WASM fetch
#      won't work over file://):
#        python3 -m http.server 8000 --directory idf_cpp_proto/tools/wasm
#      then visit http://localhost:8000/preview.html
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
SCREENS_DIR="${REPO_ROOT}/idf_cpp_proto/main/screens"
DIST_DIR="${SCRIPT_DIR}/dist"

: "${EMSCRIPTEN_MIN_VERSION:=3.1.0}"
: "${OPT_LEVEL:=-O2}"

if ! command -v em++ >/dev/null 2>&1; then
    echo "em++ not found in PATH. Install emsdk and run 'source ~/emsdk/emsdk_env.sh'." >&2
    exit 127
fi

em_version=$(em++ --version | head -n1 | sed -nE 's/.*[[:space:]]([0-9]+\.[0-9]+\.[0-9]+).*/\1/p')
if [ -z "$em_version" ]; then
    echo "Could not parse em++ --version output" >&2
    exit 1
fi
if ! printf '%s\n%s\n' "$EMSCRIPTEN_MIN_VERSION" "$em_version" | sort -VC; then
    echo "em++ ${em_version} is older than required ${EMSCRIPTEN_MIN_VERSION}" >&2
    exit 1
fi

if [ ! -f "${SCREENS_DIR}/common.cpp" ]; then
    echo "Expected ${SCREENS_DIR}/common.cpp — is REPO_ROOT=${REPO_ROOT} correct?" >&2
    exit 1
fi

mkdir -p "${DIST_DIR}"

echo "[wasm] em++ ${em_version}, opt=${OPT_LEVEL}"
echo "[wasm] compiling -> ${DIST_DIR}/btclock_datahandler.{js,wasm}"

# Include path rooted at main/ so binding.cpp can `#include "screens/…"`.
# We compile binding.cpp with -DBTCLOCK_WASM_BUILD so common.cpp (which
# the binding calls into) can be compiled unchanged — the binding's own
# forward decls match common.hpp's namespace btclock pure-logic signatures.
em++ \
    -lembind \
    -std=gnu++17 \
    "${OPT_LEVEL}" \
    -DBTCLOCK_WASM_BUILD \
    -I"${REPO_ROOT}/idf_cpp_proto/main" \
    "${SCREENS_DIR}/common.cpp" \
    "${SCRIPT_DIR}/binding.cpp" \
    -o "${DIST_DIR}/btclock_datahandler.js" \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createBtclockModule \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 \
    --no-entry

echo "[wasm] ok — open preview.html via a local HTTP server (see header comment)."
