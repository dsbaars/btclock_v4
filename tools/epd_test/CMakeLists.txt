cmake_minimum_required(VERSION 3.16)

# Minimal EPD-driver sanity test. Talks to the SSD1680 panels directly
# via the production epd_ssd1680 component, with NO wifi / data / font /
# screen-manager code in the loop. Paints a seconds counter that
# alternates normal- and inverted-polarity panels so any refresh issue
# shows up unambiguously in a photograph.
#
# Build: idf.py -C tools/epd_test -B tools/epd_test/build build
# Flash: tools/flash/flash-rev-b.sh won't work — this project produces
#        its own bin. Use:
#   curl -X POST --data-binary @tools/epd_test/build/btclock_epd_test.bin \
#     http://<ip>/upload/firmware
# …except this firmware doesn't run a webserver, so OTA only lands
# once; back to the real firmware requires USB reflash.

# Borrow the parent components/ so we don't duplicate driver sources.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Explicit allowlist — EXTRA_COMPONENT_DIRS otherwise pulls in every
# sibling component (wifi, webserver, mining_pool_*, nostr, ...). Keep
# the test binary lean + fast to rebuild by listing only the production
# components it actually links against, plus the few IDF-provided ones
# the driver code needs transitively.
set(COMPONENTS
    main
    # IDF components the driver layer pulls in
    driver esp_timer esp_log freertos esp_common
    # Production BTClock driver components under test
    i2c_bus mcp23017 pca9685 epd_ssd1680)

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btclock_epd_test)
