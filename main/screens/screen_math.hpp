// Pure-logic helpers for the screen renderers — halving schedule,
// circulating-supply math, market-cap product, HH:MM slot layout.
//
// Lives in its own header (no ESP-IDF or font.hpp deps) so host tests
// can exercise it directly from test_host/. Renderer .cpp files get
// this via common.hpp which re-includes it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {

inline constexpr uint32_t kHalvingInterval = 210000;
// Reward halves to 0 after 33 eras (50 * 0.5^33 rounds to 0 sats).
inline constexpr uint32_t kMaxHalvingEras = 33;
// 21,000,000 BTC hard cap (integer form we actually display).
inline constexpr uint64_t kMaxSupplyBtc = 21000000ULL;

// Blocks remaining until the *next* halving. At the exact halving
// block (e.g. 210000, 420000) the countdown resets to a full interval
// (210000), matching the old firmware: (210000 - (height % 210000)).
inline constexpr uint32_t HalvingCountdown(uint32_t height) {
  return kHalvingInterval - (height % kHalvingInterval);
}

// Cumulative circulating supply in whole BTC at `height`. Sums
// completed eras (each contributing `interval * reward_era`), then
// adds the partial current era (`blocks_in_era * reward_era`). Reward
// halves every era; after 33 eras the reward is 0 sats and supply is
// capped.
inline constexpr uint64_t SupplyAtBlock(uint32_t height) {
  uint64_t sats = 0;
  // 50 BTC = 5_000_000_000 sats. Use sats for integer precision.
  uint64_t reward = 5000000000ULL;
  uint32_t h = height;
  for (uint32_t era = 0; era < kMaxHalvingEras && h > 0; ++era) {
    const uint32_t in_era =
        h >= kHalvingInterval ? kHalvingInterval : h;
    sats += static_cast<uint64_t>(in_era) * reward;
    h -= in_era;
    reward >>= 1;
  }
  const uint64_t btc = sats / 100000000ULL;
  return btc > kMaxSupplyBtc ? kMaxSupplyBtc : btc;
}

// Market cap in integer `currency` units = price_int * supply_btc.
// Both inputs are already integer-rounded upstream (PriceInt, supply
// in whole BTC) so this is an exact multiply. Fits in uint64_t for all
// real-world prices (overflow would need price > 8.7e11).
inline constexpr uint64_t MarketCap(uint32_t price_int, uint32_t height) {
  return static_cast<uint64_t>(price_int) * SupplyAtBlock(height);
}

// HH:MM slots. Panel 0 carries a separate date label (rendered via
// DrawSplitText); the remaining panels carry the time digits with a
// colon separator in the slot nearest the middle. Returns digits
// indexed by `slot` (0..N-2), where '0'..'9' are digits, ':' is the
// separator, and ' ' is a blank panel.
struct ClockLayout {
  char digits[8]{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
};

// Fill a ClockLayout for a given wall-clock hour (0..23) and minute
// (0..59) across `digit_panels` slots. The last four slots are always
// "HH:MM" (leading-zero-padded); earlier slots are blank. Caller
// ensures digit_panels >= 5. If `valid` is false, all slots are ' '
// so the renderer blanks the time area (SNTP not synced yet).
ClockLayout ComputeClockLayout(bool valid, int hour, int minute,
                               size_t digit_panels);

// Right-justify a 64-bit unsigned integer into `digits[slots]`. Same
// blank-pad / leading-truncate rules as FormatDigits for uint32_t.
void FormatDigits64(uint64_t v, char* digits, std::size_t slots);

}  // namespace btclock
