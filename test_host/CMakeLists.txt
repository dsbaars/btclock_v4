# Host-side unit tests. Plain CMake, no ESP-IDF dependency.
#
# Build + run:
#   cmake -S test_host -B build-host
#   cmake --build build-host
#   ./build-host/btclock_host_tests
#
# The tests include pure-logic code from main/ and pure-logic parser
# headers from components/mining_pool_*/include. Files that pull in
# ESP-IDF headers (FreeRTOS, esp_http_client, nvs) do NOT belong here.

cmake_minimum_required(VERSION 3.16)
project(btclock_host_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
# cJSON is vendored unchanged from the ESP-IDF tree; its own warnings
# (string-conversion, sign-compare) are not actionable here.
set_source_files_properties(vendor/cJSON.c PROPERTIES
    COMPILE_FLAGS "-w")

add_executable(btclock_host_tests
    main.cpp
    test_control_validators.cpp
    test_frontlight_fade.cpp
    test_net_util.cpp
    test_partitions.cpp
    test_screen_ref_chars.cpp
    test_fee_rate.cpp
    test_btclock_data_parse.cpp
    test_nostr_parse.cpp
    test_tls_gate.cpp
    test_mining_pool_braiins.cpp
    test_mining_pool_ocean.cpp
    test_mining_pool_noderunners.cpp
    test_mining_pool_satoshi_radio.cpp
    test_mining_pool_public_pool.cpp
    test_mining_pool_ckpool.cpp
    test_mining_pool_gobrrr.cpp
    test_screen_math.cpp
    ../main/app/frontlight_fader.cpp
    ../main/screens/screen_math.cpp
    ../components/nostr/src/parser.cpp
    ../components/nostr/src/request_frame.cpp
    vendor/cJSON.c
)

target_include_directories(btclock_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/webserver/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/btclock_data/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nostr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/tls_gate/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_braiins/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_ocean/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_noderunners/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_satoshi_radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_public_pool/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_ckpool/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_gobrrr/include
)

find_package(Threads REQUIRED)
target_link_libraries(btclock_host_tests PRIVATE Threads::Threads)

# Give tests a compile-time pointer to the partition CSVs so we can test
# them without copying / duplicating paths.
target_compile_definitions(btclock_host_tests PRIVATE
    BTCLOCK_POC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

enable_testing()
add_test(NAME btclock_host_tests COMMAND btclock_host_tests)
