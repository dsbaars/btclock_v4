cmake_minimum_required(VERSION 3.16)

# --- Board variant selection ---
# Pass on the command line: `idf.py -DBTCLOCK_BOARD=REV_A build`
# Defaults to Rev B (the board currently in use on /dev/cu.usbmodem833301).
set(BTCLOCK_BOARD "REV_B" CACHE STRING "BTClock hardware variant: REV_A, REV_B or V8")

# --- Panel variant selection (orthogonal to BTCLOCK_BOARD) ---
# Same hardware can ship with different EPD geometries; rather than fork
# the board enum, expose an orthogonal -DBTCLOCK_PANEL=2_13|2_9|7_5 flag.
# 2.13" is the default for every board, but every board × panel combo
# is allowed at configure time — pin maps live on the board, panel
# geometry lives on the driver, and the EPD pin strap (CS/DC/RST/BUSY)
# is panel-agnostic. Untested combinations may need bring-up work, but
# the build system should never gate physical exploration.
set(BTCLOCK_PANEL "2_13" CACHE STRING "EPD panel variant: 2_13, 2_9 or 7_5")

string(TOLOWER "${BTCLOCK_BOARD}" _BTCLOCK_BOARD_LC)
set(_VARIANT_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${_BTCLOCK_BOARD_LC}")
if(NOT EXISTS "${_VARIANT_DEFAULTS}")
    message(FATAL_ERROR
        "No sdkconfig.defaults.${_BTCLOCK_BOARD_LC} for BTCLOCK_BOARD='${BTCLOCK_BOARD}'. "
        "Add one next to sdkconfig.defaults or pick a valid variant.")
endif()

# Validate the panel token only — every board × panel combo is allowed.
if(NOT BTCLOCK_PANEL MATCHES "^(2_13|2_9|7_5)$")
    message(FATAL_ERROR
        "Unknown BTCLOCK_PANEL='${BTCLOCK_PANEL}'. Use 2_13, 2_9 or 7_5.")
endif()

# Surface the panel choice to every component as a compile definition —
# the driver factory keys the unique_ptr<IEpdPanel> dispatch off this.
add_compile_definitions(BTCLOCK_PANEL_${BTCLOCK_PANEL}=1)

# Same idea for the board — components outside main need to gate optional
# assets (e.g. fonts dropping Merriweather on the 4MB Rev A) by board.
# main/CMakeLists.txt sets the same flag on its own target as well; this
# is the project-wide emit.
add_compile_definitions(BTCLOCK_BOARD_${BTCLOCK_BOARD}=1)

# Capability flags — set ONCE per board variant, consumed everywhere
# (main + components). Prefer capability over board: a future landscape
# variant that happens to share Rev B's frontlight hardware should
# light up via BTCLOCK_HAS_FRONTLIGHT alone, no per-board enumeration
# in the consumer. Flags match the constexpr `kHasFrontlight` /
# `kHasBh1750` in main/board/board_*.hpp.
if(BTCLOCK_BOARD STREQUAL "REV_B")
    add_compile_definitions(BTCLOCK_HAS_FRONTLIGHT=1)
    add_compile_definitions(BTCLOCK_HAS_BH1750=1)
else()
    add_compile_definitions(BTCLOCK_HAS_FRONTLIGHT=0)
    add_compile_definitions(BTCLOCK_HAS_BH1750=0)
endif()

# String slugs used by init_control_api.cpp to synthesise OTA asset
# names (`btclock_<board>[_<panel>]_ota.bin` /
# `..._webui.bin`) without a #if pyramid. The suffix is empty for the
# default 2.13" panel so the common case stays plain
# `btclock_rev_b_ota.bin`; explicit suffixes only appear on non-default
# panels. Adding a new panel here is a one-line edit instead of a
# board × panel matrix expansion in init_control_api.cpp.
add_compile_definitions(BTCLOCK_BOARD_SLUG="${_BTCLOCK_BOARD_LC}")
if(BTCLOCK_PANEL STREQUAL "2_13")
    add_compile_definitions(BTCLOCK_PANEL_ASSET_SUFFIX="")
elseif(BTCLOCK_PANEL STREQUAL "2_9")
    add_compile_definitions(BTCLOCK_PANEL_ASSET_SUFFIX="_29")
elseif(BTCLOCK_PANEL STREQUAL "7_5")
    add_compile_definitions(BTCLOCK_PANEL_ASSET_SUFFIX="_75")
endif()

# --- Optional on-device diagnostics (all default OFF, zero-cost when OFF) ---
#
# BTCLOCK_DIAG_UDP_LOG (general): install an esp_log -> UDP:9999 broadcast
# mirror after network bring-up so on-device logs are readable from the
# host — the USB-Serial-JTAG console is unreadable from the dev machine,
# so this is the go-to way to read ANY ESP_LOG* output live. Capture with
# a UDP socket bound to :9999. Build is WARN-max, so only ESP_LOGW+ shows.
#   idf.py -DBTCLOCK_DIAG_UDP_LOG=ON ... build
option(BTCLOCK_DIAG_UDP_LOG
    "Mirror esp_log over UDP:9999 (host-readable on-device diagnostics)" OFF)
#
# BTCLOCK_DIAG_NWC_FLASH (specific): compile in four ESP_LOGW trace points
# for the NWC payment / frontlight double-flash path (notif dispatch kind,
# on_payment fan-out, flash trigger, frontlight flash dequeue). Implies the
# UDP log mirror so the trace is actually readable off-device.
#   idf.py -DBTCLOCK_DIAG_NWC_FLASH=ON ... build
option(BTCLOCK_DIAG_NWC_FLASH
    "Compile in NWC payment / frontlight-flash trace points (implies UDP log)" OFF)

if(BTCLOCK_DIAG_NWC_FLASH)
    add_compile_definitions(BTCLOCK_DIAG_NWC_FLASH=1)
    set(BTCLOCK_DIAG_UDP_LOG ON)
endif()
if(BTCLOCK_DIAG_UDP_LOG)
    add_compile_definitions(BTCLOCK_DIAG_UDP_LOG=1)
endif()

# Base defaults first, per-variant overrides on top (later files win).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${_VARIANT_DEFAULTS}")

# Bake a git-derived version string into the firmware so esp_app_desc_t
# `version` is meaningful for both tagged releases and untagged snapshots.
# Tagged commits surface as e.g. "4.0.0-beta.1" (or "4.0.0-beta.1-dirty"
# with local edits); untagged commits fall back to "<short-sha>" via
# `--always`. The match pattern `[0-9]*` is semver-shaped (no `v`
# prefix) — same convention `.forgejo/workflows/release.yaml` keys
# its tag trigger off of. IDF picks PROJECT_VER up before project()
# and embeds it in the app image header — readable on-device via
# esp_app_get_description() and surfaced through /api/version,
# /api/settings.gitRev, the mDNS TXT record, and the provisioning +
# debug panels.
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty --match "[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE _BTCLOCK_GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _BTCLOCK_GIT_VERSION_RC)
    if(_BTCLOCK_GIT_VERSION_RC EQUAL 0 AND _BTCLOCK_GIT_VERSION)
        set(PROJECT_VER "${_BTCLOCK_GIT_VERSION}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btclock_v4)

# Link-time optimisation — gated on BTCLOCK_LTO=ON. Saves ~10–40 KiB
# of .text on Rev A by letting the linker prune cross-TU unreachable
# code. Off by default because gcc-LTO occasionally breaks IDF's
# IRAM_ATTR section pinning under aggressive inlining and the failure
# mode (random crash on RX-DMA path) is far worse than the size win.
# Rev A is the only variant tight on flash today; the per-variant
# defaults file flips this on for REV_A only.
if(BTCLOCK_LTO)
    idf_build_set_property(COMPILE_OPTIONS
        "-flto=auto;-ffat-lto-objects;-fno-asynchronous-unwind-tables;-fno-unwind-tables;-fmerge-all-constants"
        APPEND)
    idf_build_set_property(LINK_OPTIONS "-flto=auto" APPEND)
endif()
