cmake_minimum_required(VERSION 3.16)

# --- Board variant selection ---
# Pass on the command line: `idf.py -DPOC_BOARD=REV_A build`
# Defaults to Rev B (the board currently in use on /dev/cu.usbmodem833301).
set(POC_BOARD "REV_B" CACHE STRING "BTClock hardware variant: REV_A or REV_B")

string(TOLOWER "${POC_BOARD}" _POC_BOARD_LC)
set(_VARIANT_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${_POC_BOARD_LC}")
if(NOT EXISTS "${_VARIANT_DEFAULTS}")
    message(FATAL_ERROR
        "No sdkconfig.defaults.${_POC_BOARD_LC} for POC_BOARD='${POC_BOARD}'. "
        "Add one next to sdkconfig.defaults or pick a valid variant.")
endif()

# Base defaults first, per-variant overrides on top (later files win).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${_VARIANT_DEFAULTS}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btclock_idf_proto)
