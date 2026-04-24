// Pure-logic helpers for the BH1750 driver. Split out so host tests can
// link the conversion without dragging in ESP-IDF I2C headers.

#pragma once

#include <cstdint>

namespace btclock {
namespace bh1750 {

// Datasheet, section "How to calculate illuminance": lx = counts / 1.2
// for the H-resolution modes. Returned as float — the caller decides
// how much precision to surface through the API.
constexpr float RawToLux(uint16_t raw) {
  return static_cast<float>(raw) / 1.2f;
}

}  // namespace bh1750
}  // namespace btclock
