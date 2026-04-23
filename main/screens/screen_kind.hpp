// Screen-kind enum, split out of screens.hpp so pure-logic helpers
// (panel_texts, host tests) can include it without pulling in the EPD
// driver or font headers screens.hpp drags in.

#pragma once

#include <cstdint>

namespace btclock {

// Which top-level screen is currently being displayed. Screen rotation
// and button navigation cycle through these.
enum class ScreenType : uint8_t {
  kBlockHeight,
  kMoscowTime,
  kBtcPrice,
  kBlockFeeRate,
  kClock,
  kHalving,
  kBitcoinSupply,
  kMarketCap,
};

}  // namespace btclock
