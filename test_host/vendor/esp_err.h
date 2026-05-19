// Host-test stub for esp_err.h. Only the minimum shape used by the
// pure-logic .cpps under test (DataHub, etc.) — enough for them to
// compile against the host toolchain without dragging in ESP-IDF.

#pragma once

#include <cstdint>

using esp_err_t = int32_t;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
