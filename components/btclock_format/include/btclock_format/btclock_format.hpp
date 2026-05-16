// btclock_format — panel-agnostic number formatting + Bitcoin-domain
// math shared between every BTClock variant.
//
// Pulled out of main/screens/screen_math.{hpp,cpp} so a future
// landscape (single 2.9" panel) variant under main_landscape/ can
// reuse the same suffix vocabulary ("$95.4K", "1.2M sats"), halving
// math, and mining-pool layouts without dragging in the 7×portrait
// multi-panel layout helpers (those still live in screens/).
//
// Pure C++ — no ESP-IDF, no FreeRTOS, no font.hpp. Host tests build
// it directly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace btclock {

// --- Bitcoin domain constants ---

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

// Block subsidy (sats) at `height`. Starts at 50 BTC (5_000_000_000
// sats), halves every kHalvingInterval blocks. Returns 0 once the
// reward shifts past 0 (after kMaxHalvingEras eras).
inline constexpr uint64_t BlockRewardSats(uint32_t height) {
  const uint32_t era = height / kHalvingInterval;
  if (era >= kMaxHalvingEras) return 0;
  return 5000000000ULL >> era;
}

// Cumulative circulating supply in whole BTC at `height`. Sums
// completed eras (each contributing `interval * reward_era`), then
// adds the partial current era. Reward halves every era; after 33
// eras the reward is 0 sats and supply is capped.
inline constexpr uint64_t SupplyAtBlock(uint32_t height) {
  uint64_t sats = 0;
  uint64_t reward = 5000000000ULL;
  uint32_t h = height;
  for (uint32_t era = 0; era < kMaxHalvingEras && h > 0; ++era) {
    const uint32_t in_era = h >= kHalvingInterval ? kHalvingInterval : h;
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

// --- Generic number formatters ---

// Right-justify a 64-bit unsigned integer into `digits[slots]`. Pads
// the head with ' '. If the decimal form is longer than `slots`, the
// leading digits are truncated (keep the rightmost `slots`).
void FormatDigits64(uint64_t v, char* digits, std::size_t slots);

// K/M/B/T/Q suffix form of an integer, e.g. 1_020_825_000_000 → "1.02T".
// `num_characters` is the total width budget (including the suffix
// letter): the more space, the more decimal places are packed in. When
// `mow_mode` is set, the thousands scale jumps directly to "M" (MOW
// units); otherwise "K" is used below 1e6. Ports
// lib/btclock/utils.cpp::formatNumberWithSuffix without any on-device
// FreeRTOS dependencies so host tests can exercise it directly.
std::string FormatNumberWithSuffix(uint64_t num, int num_characters = 4,
                                   bool mow_mode = false);

// Format a zap-like sats amount (Nostr zap, NWC balance, NWC payment
// notify) into the string painted on the trailing panels.
// `max_int_cells` is the panel-tail budget for the integer rendering —
// when the raw integer fits, it is preferred over the suffix form so a
// 1000-sat zap reads "1000" not "1.0K". When it doesn't fit, falls
// back to K / M / B suffix (uppercase — matches FormatNumberWithSuffix
// above so the BTC-ticker, market-cap, supply, zap, and NWC paths
// share one suffix vocabulary):
// 1000..999_999 → "NK" / "N.NK", >= 1_000_000 → "NM" / "N.NM",
// >= 1_000_000_000 → "NB". Negative / missing → "?".
std::string FormatZapAmount(const std::optional<int64_t>& amount_sats,
                            std::size_t max_int_cells);

// --- Panel-agnostic layout helpers ---
//
// These take a numeric value and produce a small struct ({value,
// unit}). The renderer decides how to paint them onto panels (a 7×
// portrait variant uses one cell per field; a 1× landscape variant
// can render both on the same panel).

// Halving countdown breakdown: the number of whole years, days, hours
// and minutes until the next halving, assuming 10-minute blocks. Ports
// the floor-arithmetic cascade from lib/btclock/data_handler.cpp's
// parseHalvingCountdown time-mode branch (asBlocks=false).
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
// that as the "no data yet" indicator. `max_chars` caps the digit
// width (including the '.') — the renderer passes the count of digit
// panels. Output matches the old firmware utils.cpp parseHashrateString
// so the KH/MH/GH/TH/PH/EH/ZH ladder and rounding are identical.
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
// with the whole-BTC count. Returns the value string and the unit
// label the renderer should print on the trailing panel.
struct MiningPoolEarningsLayout {
  std::string value;       // "21000", "12.3K", "1.50M", "1"
  std::string unit_label;  // "SATS" or "BTC" depending on magnitude
  bool valid = false;      // false when daily_sats is absent / negative
};
MiningPoolEarningsLayout LayoutMiningPoolEarnings(int64_t daily_sats);

}  // namespace btclock
