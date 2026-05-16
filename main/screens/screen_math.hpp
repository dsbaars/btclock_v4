// Panel-multiplexed layout helpers — the bits that *are* tied to the
// 7×portrait BTClock screen geometry. Panel-agnostic formatters and
// Bitcoin-domain math (FormatNumberWithSuffix, FormatZapAmount,
// SupplyAtBlock, HalvingCountdownBreakdown, etc.) live in
// components/btclock_format/ so a future landscape variant can reuse
// them without dragging in the multi-panel slot vocabulary below.
//
// This header re-exports the btclock_format surface so existing
// `#include "screens/screen_math.hpp"` callers (and common.hpp's
// transitive consumers) keep compiling unchanged.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "btclock_format/btclock_format.hpp"

namespace btclock {

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

// Block-height screen layout: when the decimal form of `height` needs
// at least `panels` digits (7 on a 7-panel board) the "BLOCK/HEIGHT"
// label is dropped and every panel carries a digit — otherwise panel 0
// is the label and the remaining panels hold right-justified digits.
// Matches old firmware lib/btclock/data_handler.cpp parseBlockHeight.
inline bool BlockHeightDropsLabel(uint32_t height, std::size_t panels) {
  char buf[16];
  const int len =
      std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(height));
  return len >= 0 && static_cast<std::size_t>(len) >= panels;
}

}  // namespace btclock
