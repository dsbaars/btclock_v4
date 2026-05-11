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

# Optional sanitizer build. Default OFF so the standard
# `cmake -S test_host -B build-host` path is unchanged.
option(BTCLOCK_HOST_TESTS_SANITIZE "Build host tests with ASan + UBSan" OFF)
if(BTCLOCK_HOST_TESTS_SANITIZE)
    # -O1 is the standard sanitizer compromise: -O0 is glacial and misses
    # some optimization-dependent bugs; -O2+ folds away frame info ASan
    # needs for symbolicated stack traces.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -O1 -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Optional gcov coverage build. Default OFF; CI runs this as an
# informational job and uploads the HTML report as an artifact.
option(BTCLOCK_HOST_TESTS_COVERAGE
       "Build host tests with gcov instrumentation" OFF)
if(BTCLOCK_HOST_TESTS_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# cJSON is vendored unchanged from the ESP-IDF tree; its own warnings
# (string-conversion, sign-compare) are not actionable here.
set_source_files_properties(vendor/cJSON.c PROPERTIES
    COMPILE_FLAGS "-w")

# Vendored libsecp256k1 — same suppression rationale as cJSON. Defines
# match the firmware build (components/secp256k1/CMakeLists.txt) so the
# host verifier hashes-and-verifies identically to on-target.
set_source_files_properties(
    ../components/secp256k1/src/secp256k1.c
    ../components/secp256k1/src/precomputed_ecmult.c
    ../components/secp256k1/src/precomputed_ecmult_gen.c
    PROPERTIES
    COMPILE_FLAGS "-w"
    COMPILE_DEFINITIONS "ENABLE_MODULE_EXTRAKEYS=1;ENABLE_MODULE_SCHNORRSIG=1;ECMULT_WINDOW_SIZE=4")

add_executable(btclock_host_tests
    main.cpp
    test_control_validators.cpp
    test_frontlight_fade.cpp
    test_frontlight_ambient_policy.cpp
    test_frontlight_stagger.cpp
    test_mime.cpp
    test_net_util.cpp
    test_hostname.cpp
    test_partitions.cpp
    test_screen_ref_chars.cpp
    test_fee_rate.cpp
    test_btclock_data_parse.cpp
    test_btclock_subscribe.cpp
    test_nostr_parse.cpp
    test_nostr_verify.cpp
    test_tls_gate.cpp
    test_mining_pool_braiins.cpp
    test_mining_pool_ocean.cpp
    test_mining_pool_noderunners.cpp
    test_mining_pool_satoshi_radio.cpp
    test_mining_pool_public_pool.cpp
    test_mining_pool_ckpool.cpp
    test_mining_pool_gobrrr.cpp
    test_mining_pool_foundry.cpp
    test_bitaxe_parser.cpp
    test_bitaxe_panel_texts.cpp
    test_screen_math.cpp
    test_webui_upload_bounds.cpp
    test_ota_upload_bounds.cpp
    test_ota_progress.cpp
    test_ota_quiesce.cpp
    test_timezone.cpp
    test_led_curves.cpp
    test_led_color_parse.cpp
    test_led_prefs.cpp
    ../main/io/led_prefs.cpp
    test_datahandler_parity.cpp
    test_settings_api.cpp
    test_settings_catalogs.cpp
    test_nostr_config.cpp
    test_dnd.cpp
    test_screen_rotation.cpp
    test_screen_slot_map.cpp
    test_rotation_plan.cpp
    test_panel_texts.cpp
    test_price_layout.cpp
    test_show_text_parse.cpp
    test_preview_packet.cpp
    test_gzip_header_parse.cpp
    test_symbol_mode.cpp
    test_mdi_custom_cell.cpp
    test_url_decode.cpp
    test_sse_frame.cpp
    test_heap_metrics.cpp
    test_nostr_relay_share.cpp
    test_zap_id_lru.cpp
    test_sats_variant.cpp
    test_auth_gate_logic.cpp
    test_wifi_outage_watchdog.cpp
    test_bh1750_math.cpp
    test_light_metrics.cpp
    test_font_roles.cpp
    test_font_bundle_map.cpp
    test_markdown_parse.cpp
    test_fit_text_px.cpp
    test_landscape_rotation.cpp
    test_refresh_policy.cpp
    test_build_time.cpp
    test_block_event_policy.cpp
    test_block_height_validator.cpp
    test_vertical_desc.cpp
    test_sources_uri.cpp
    test_btclock_lastblock_uri.cpp
    ../components/btclock_data/btclock_lastblock_uri.cpp
    test_btclock_currencies_uri.cpp
    test_btclock_currencies_parse.cpp
    test_sats_per_currency_layout.cpp
    test_proxy_framing.cpp
    test_proxy_bypass.cpp
    ../components/btclock_data/btclock_currencies_uri.cpp
    ../components/btclock_data/btclock_currencies_parse.cpp
    ../components/proxy_transport/src/proxy_framing.cpp
    ../components/proxy_transport/src/proxy_bypass.cpp
    ../main/sources/sources_uri.cpp
    ../main/app/refresh_policy.cpp
    ../main/io/frontlight_fader.cpp
    ../main/io/frontlight_ambient_policy.cpp
    ../main/io/frontlight_stagger.cpp
    ../main/io/led_curves.cpp
    ../main/screens/screen_math.cpp
    ../main/screens/panel_texts.cpp
    ../main/screens/assets/pool_logos.cpp
    ../main/screens/assets/pool_logo_cache_host.cpp
    ../components/bitaxe/src/bitaxe_parser.cpp
    ../components/nostr/src/parser.cpp
    ../components/nostr/src/request_frame.cpp
    ../components/nostr/src/event_verify.cpp
    ../components/secp256k1/src/secp256k1.c
    ../components/secp256k1/src/precomputed_ecmult.c
    ../components/secp256k1/src/precomputed_ecmult_gen.c
    ../components/webserver/mime.cpp
    ../components/webserver/show_text_parse.cpp
    ../components/fonts/mdi_custom_cell.cpp
    ../components/webserver/url_decode.cpp
    ../components/webserver/sse_frame.cpp
    ../components/webserver/auth_gate_logic.cpp
    ../components/settings/settings_api.cpp
    ../components/settings/build_time.cpp
    ../components/settings/nostr_config.cpp
    ../components/dnd/dnd_window.cpp
    vendor/cJSON.c
)

target_include_directories(btclock_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/webserver/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/btclock_data/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/data_core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nostr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/secp256k1/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/secp256k1/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/tls_gate/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/net_util/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_braiins/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_ocean/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_noderunners/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_satoshi_radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_public_pool/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_ckpool/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_gobrrr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mining_pool_foundry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bitaxe/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/btclock_fs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/settings/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dnd/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/fonts/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bh1750/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/proxy_transport/include
)

find_package(Threads REQUIRED)
target_link_libraries(btclock_host_tests PRIVATE Threads::Threads)

# Give tests a compile-time pointer to the partition CSVs so we can test
# them without copying / duplicating paths.
target_compile_definitions(btclock_host_tests PRIVATE
    BTCLOCK_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

enable_testing()
add_test(NAME btclock_host_tests COMMAND btclock_host_tests)

# --- libFuzzer harness for the Nostr parser --------------------------
# Optional fuzz target. Off by default so `cmake -S test_host -B
# build-host` keeps producing the doctest binary unchanged. Turning
# this on requires a clang toolchain — libFuzzer is clang-only — and
# adds a separate executable that loops `LLVMFuzzerTestOneInput`.
#
#   cmake -S test_host -B build-fuzz -DBTCLOCK_FUZZ=ON \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz
#   # Run with a workdir that's NOT the seed dir — libFuzzer writes
#   # every interesting discovery into the FIRST corpus dir on the
#   # command line, so pointing it at the version-controlled seed dir
#   # would pollute the tree with thousands of hash-named files.
#   mkdir -p build-fuzz/corpus
#   ./build-fuzz/nostr_parser_fuzzer build-fuzz/corpus \
#       test_host/fuzz_corpus/nostr_parser/ \
#       -max_total_time=300
#
# (Apple clang ships without libFuzzer; on macOS use Homebrew LLVM —
# CC=/opt/homebrew/opt/llvm/bin/clang.)
#
# Sanitizers (-fsanitize=fuzzer,address,undefined) are baked into the
# fuzzer target only, so the regular host-test build stays free of
# sanitizer overhead. ASan + UBSan together catch the practical bug
# classes a hand-rolled JSON walker is prone to: OOB reads on truncated
# escapes, signed overflow on number parsing, use-after-free on string
# returns. A run that exits 0 after `-max_total_time` means "no findings
# in this many seconds"; any crash leaves a `crash-<sha>` reproducer in
# CWD that gets seeded into the corpus on next run.
option(BTCLOCK_FUZZ "Build libFuzzer harnesses (clang only)" OFF)
if(BTCLOCK_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(FATAL_ERROR
            "BTCLOCK_FUZZ=ON requires clang/clang++ "
            "(libFuzzer is not implemented in gcc).")
    endif()

    # Helper macro: every harness wants the same sanitizer + libFuzzer
    # link flags, so factor them out to keep the per-target stanzas
    # to just sources + includes.
    set(_FUZZ_OPTS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
                   -O1 -g)

    add_executable(nostr_parser_fuzzer
        fuzz_nostr_parser.cpp
        ../components/nostr/src/parser.cpp
    )
    target_include_directories(nostr_parser_fuzzer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/nostr/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/data_core/include
    )
    target_compile_options(nostr_parser_fuzzer PRIVATE ${_FUZZ_OPTS})
    target_link_options(nostr_parser_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)

    # url_decode covers the in-place query-string decoder hit by every
    # HTTP request that takes a `?key=value` parameter. Tiny TU, big
    # blast radius — bug here = potential OOB read on every endpoint.
    add_executable(url_decode_fuzzer
        fuzz_url_decode.cpp
        ../components/webserver/url_decode.cpp
    )
    target_include_directories(url_decode_fuzzer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/webserver/include
    )
    target_compile_options(url_decode_fuzzer PRIVATE ${_FUZZ_OPTS})
    target_link_options(url_decode_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()
