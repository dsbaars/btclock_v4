# Host-side unit tests. Plain CMake, no ESP-IDF dependency.
#
# Build + run:
#   cmake -S test_host -B build-host
#   cmake --build build-host
#   ./build-host/btclock_host_tests
#
# The tests include pure-logic code from main/ directly — only files that
# compile without any ESP-IDF headers qualify. Keep it that way.

cmake_minimum_required(VERSION 3.16)
project(btclock_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

add_executable(btclock_host_tests
    main.cpp
    test_net_util.cpp
    test_partitions.cpp
)

target_include_directories(btclock_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Give tests a compile-time pointer to the partition CSVs so we can test
# them without copying / duplicating paths.
target_compile_definitions(btclock_host_tests PRIVATE
    BTCLOCK_POC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

enable_testing()
add_test(NAME btclock_host_tests COMMAND btclock_host_tests)
