// Pure-logic helpers for the screen renderers — halving schedule,
// circulating-supply math, market-cap product, HH:MM slot layout.
//
// Lives in its own header (no ESP-IDF or font.hpp deps) so host tests
// can exercise it directly from test_host/. Renderer .cpp files get
// this via common.hpp which re-includes it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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
// When `hide_leading_zero=true` and hour < 10, the tens-of-hours slot
// is blanked so "07:05" renders as " 7:05". Minute digits are always
// zero-padded (matches user-visible formatting: "7:05", never "7:5").
ClockLayout ComputeClockLayout(bool valid, int hour, int minute,
                               size_t digit_panels,
                               bool hide_leading_zero = false);

// Right-justify a 64-bit unsigned integer into `digits[slots]`. Same
// blank-pad / leading-truncate rules as FormatDigits for uint32_t.
void FormatDigits64(uint64_t v, char* digits, std::size_t slots);

// 3-digit-group layout shared by Market Cap (small-chars mode) and
// Bitcoin Supply (small-chars mode). Splits `value` into groups of 3
// digits right-aligned across the trailing slots, prepends the optional
// `ccy_cell` separator (for Market Cap), and leaves earlier slots
// blank. Output vector has exactly `slots` entries; caller decides
// whether `slots` includes a label panel (for panel-text mirror) or
// excludes it (for the on-device renderer iterating over the digit
// panels). Ports lib/btclock/data_handler.cpp::parseMarketCap and
// parseBitcoinSupply small-chars branches so the device and /api/status
// mirror agree byte-for-byte.
std::vector<std::string> SmallCharsGroups(uint64_t value,
                                          const std::string& ccy_cell,
                                          std::size_t slots);

// K/M/B/T/Q suffix form of an integer, e.g. 1_020_825_000_000 → "1.02T".
// `num_characters` is the total width budget (including the suffix
// letter): the more space, the more decimal places are packed in. When
// `mow_mode` is set, the thousands scale jumps directly to "M" (MOW
// units); otherwise "K" is used below 1e6. Ports
// lib/btclock/utils.cpp::formatNumberWithSuffix without any on-device
// FreeRTOS dependencies so host tests can exercise it directly.
std::string FormatNumberWithSuffix(uint64_t num, int num_characters = 4,
                                   bool mow_mode = false);

// Block-height screen layout: when the decimal form of `height` needs
// at least `panels` digits (7 on a 7-panel board) the "BLOCK/HEIGHT"
// label is dropped and every panel carries a digit — otherwise panel 0
// is the label and the remaining panels hold right-justified digits.
// Matches old firmware lib/btclock/data_handler.cpp parseBlockHeight.
inline bool BlockHeightDropsLabel(uint32_t height, std::size_t panels) {
  char buf[16];
  const int len = std::snprintf(buf, sizeof(buf), "%u",
                                static_cast<unsigned>(height));
  return len >= 0 && static_cast<std::size_t>(len) >= panels;
}

// Halving countdown breakdown: the number of whole years, days, hours
// and minutes until the next halving, assuming 10-minute blocks. Ports
// the floor-arithmetic cascade from lib/btclock/data_handler.cpp's
// parseHalvingCountdown time-mode branch (asBlocks=false). Separated out
// so the renderer and the panel-texts mirror compute identical values.
struct HalvingTimeBreakdown {
  uint32_t years = 0;
  uint32_t days = 0;
  uint32_t hours = 0;
  uint32_t minutes = 0;
};
HalvingTimeBreakdown HalvingCountdownBreakdown(uint32_t block_height);

// Mining pool hashrate parse. The data source lands the pool's raw
// hashrate string (integer H/s as reported, with no suffix) in the
// snapshot; the renderer rescales to the highest unit that still leaves
// at least one significant digit and trims trailing zeros. `value` on
// empty / invalid input is "0" with "H/S" label — the renderer uses
// that as the "no data yet" indicator. `max_chars` caps the digit width
// (including the '.') — the renderer passes the count of digit panels.
// Output matches the old firmware utils.cpp parseHashrateString so the
// KH/MH/GH/TH/PH/EH/ZH ladder and rounding behaviour are identical.
struct MiningPoolHashrateLayout {
  std::string value;  // "1.3", "645", "123.4" etc.; empty/unknown → "0"
  std::string unit;   // "PH/S", "TH/S", … ; fallback "H/S"
};
MiningPoolHashrateLayout LayoutMiningPoolHashrate(
    const std::string& hashrate_raw, unsigned int max_chars = 4);

// Mining pool daily earnings format. Mirrors old firmware
// parseMiningPoolStatsDailyEarnings — sats below 10k render verbatim,
// 10K..99.9K and 1M..99.9M get one decimal, 100K..999K and 1M..9.99M
// get integer+K/M suffix, and 1 BTC/day or more drops to a BTC label
// with the whole-BTC count. Returns the value string and the unit label
// the renderer should print on the trailing panel.
struct MiningPoolEarningsLayout {
  std::string value;        // "21000", "12.3K", "1.50M", "1"
  std::string unit_label;   // "SATS" or "BTC" depending on magnitude
  bool valid = false;       // false when daily_sats is absent / negative
};
MiningPoolEarningsLayout LayoutMiningPoolEarnings(int64_t daily_sats);

}  // namespace btclock
