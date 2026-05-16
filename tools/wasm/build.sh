#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh — compile the firmware's screen stack to WebAssembly.
#
# Two bindings families ship in the same blob (see binding.cpp):
#   - parse*               : text-mode, pure-logic helpers only
#                            (main/screens/common.cpp + screen_math.cpp +
#                             fee_rate_layout.hpp).
#   - render*FrameBuffer   : pixel-accurate, runs the real template-on-N
#                            screen renderers (main/screens/block_height.cpp
#                            et al) against an in-memory EpdPanel shim
#                            (tools/wasm/wasm_panel.hpp) + the full font
#                            stack (components/fonts/font.cpp + stb_truetype
#                            + a generated ttf_blobs_wasm.cpp).
#
# How to run:
#   1. Install emsdk (https://emscripten.org/docs/getting_started/downloads.html)
#      or `brew install emscripten`.
#   2. If using emsdk:  source ~/emsdk/emsdk_env.sh
#   3. Run:              ./tools/wasm/build.sh
#   4. Serve:            python3 -m http.server 8000 --directory tools/wasm
#      then visit        http://localhost:8000/preview.html
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# tools/wasm/ → repo root is two levels up. main/, components/, etc.
# live at the top of the tree.
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SCREENS_DIR="${REPO_ROOT}/main/screens"
MAIN_DIR="${REPO_ROOT}/main"
FONTS_DIR="${REPO_ROOT}/components/fonts"
DATA_CORE_DIR="${REPO_ROOT}/components/data_core"
FORMAT_DIR="${REPO_ROOT}/components/btclock_format"
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

# Regenerate the TTF blob sources. Cheap (< 80 KB of .ttf total, output
# is a ~450 KB .cpp). Under BTCLOCK_WASM_BUILD font.hpp picks up these
# symbols in place of ESP-IDF EMBED_FILES.
TTF_BLOB_CPP="${DIST_DIR}/ttf_blobs_wasm.cpp"
python3 "${SCRIPT_DIR}/gen_font_blobs.py" \
    "${FONTS_DIR}/assets" "${TTF_BLOB_CPP}"

echo "[wasm] em++ ${em_version}, opt=${OPT_LEVEL}"
echo "[wasm] compiling -> ${DIST_DIR}/btclock_datahandler.{js,wasm}"

# Include roots:
#   main/                         — so `#include "screens/…"` works and
#                                   `#include "fonts_app.hpp"` resolves.
#   main/screens/                 — for common.hpp's `#include "wasm_panel.hpp"`
#                                   (we drop wasm_panel.hpp on this path below).
#   components/fonts/include      — font.hpp, mdi_codepoints.hpp
#   components/fonts              — stb_truetype.h (PRIV_INCLUDE_DIRS on device)
#   components/data_core/include  — data_core/snapshot.hpp (consumed by the
#                                   bitaxe / mining-pool / nostr-zap renderers
#                                   via `DataSnapshot::PoolStats` &c). On
#                                   device the ESP-IDF component CMakeLists
#                                   auto-adds this via INCLUDE_DIRS.
#   tools/wasm                    — the shim EpdPanel header.
em++ \
    -lembind \
    -std=gnu++20 \
    "${OPT_LEVEL}" \
    -DBTCLOCK_WASM_BUILD \
    -I"${MAIN_DIR}" \
    -I"${FONTS_DIR}/include" \
    -I"${FONTS_DIR}" \
    -I"${DATA_CORE_DIR}/include" \
    -I"${FORMAT_DIR}/include" \
    -I"${SCRIPT_DIR}" \
    "${FORMAT_DIR}/btclock_format.cpp" \
    "${SCREENS_DIR}/common.cpp" \
    "${SCREENS_DIR}/screen_math.cpp" \
    "${SCREENS_DIR}/panel_texts.cpp" \
    "${SCREENS_DIR}/block_height.cpp" \
    "${SCREENS_DIR}/btc_price.cpp" \
    "${SCREENS_DIR}/moscow_time.cpp" \
    "${SCREENS_DIR}/fee_rate.cpp" \
    "${SCREENS_DIR}/clock.cpp" \
    "${SCREENS_DIR}/debug.cpp" \
    "${SCREENS_DIR}/halving.cpp" \
    "${SCREENS_DIR}/bitcoin_supply.cpp" \
    "${SCREENS_DIR}/market_cap.cpp" \
    "${SCREENS_DIR}/mining_pool.cpp" \
    "${SCREENS_DIR}/assets/pool_logos.cpp" \
    "${SCREENS_DIR}/assets/bitaxe_logo.cpp" \
    "${SCREENS_DIR}/bitaxe.cpp" \
    "${SCREENS_DIR}/nostr_zap.cpp" \
    "${SCREENS_DIR}/nwc_balance.cpp" \
    "${MAIN_DIR}/fonts_app.cpp" \
    "${FONTS_DIR}/font.cpp" \
    "${FONTS_DIR}/stb_truetype_impl.c" \
    "${TTF_BLOB_CPP}" \
    "${SCRIPT_DIR}/binding.cpp" \
    "${SCRIPT_DIR}/font_wasm_aa.cpp" \
    "${SCRIPT_DIR}/pool_logos_wasm_stub.cpp" \
    -o "${DIST_DIR}/btclock_datahandler.js" \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createBtclockModule \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 \
    --no-entry

echo "[wasm] ok — open preview.html via a local HTTP server (see header comment)."
