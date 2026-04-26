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

# Base defaults first, per-variant overrides on top (later files win).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${_VARIANT_DEFAULTS}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btclock_idf_proto)
