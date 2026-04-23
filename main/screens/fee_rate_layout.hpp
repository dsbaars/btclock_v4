// Pure-logic layout helper for the block-fee-rate screen.
//
// Kept in a header (not common.hpp) so the host-side doctest can include
// it without pulling in ESP-IDF headers. The matching renderer lives in
// fee_rate.cpp and delegates the digit positioning to `LayoutFeeRate`.
//
// Parity note (old firmware, `parseBlockFees` in lib/btclock/data_handler.cpp):
//   - Panel 0: "FEE/RATE" label
//   - Panels 1..N-2: integer fee digits, right-justified, ' ' for blanks
//   - Panel N-1: "sat/vB" unit text
// Our renderer currently omits the "sat/vB" unit panel — no dedicated
// glyph exists in the fonts component yet and the Antonio subset's text
// at that panel size looked cramped in bring-up tests. Tracked in the
// same beads issue; to add it later, shrink kDigitPanels by 1 here and
// in the renderer and draw the unit on the final panel.
//
// The layout is integer-only. Decimal (`blockfee2`) is tracked by the
// separate beads issue btclock_v3_fci-znf and will need its own helper.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace btclock {

// Number of digit panels consumed by the fee-rate layout. Panel 0 is
// always the label; panels 1..N-1 are digits. Keep this in sync with
// the renderer's template parameter. When a unit-symbol panel gets
// added on panel N-1, shrink this to N-2 (see parity note above).
template <size_t N>
inline constexpr size_t kFeeRateDigitPanels = N - 1;

// Right-justify the decimal form of `fee_sats_vb` into `digits`.
// Leading positions get ' '. If `fee_sats_vb < 0`, all positions are
// left as ' ' (not-yet-received state; don't clamp to 0 because that
// would lie about the data). On overflow (value wider than available
// slots), leading digits are truncated.
template <size_t Slots>
inline void LayoutFeeRate(int32_t fee_sats_vb,
                          std::array<char, Slots>& digits) {
  for (size_t i = 0; i < Slots; ++i) digits[i] = ' ';
  if (fee_sats_vb < 0) return;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(fee_sats_vb));
  const size_t len = std::strlen(buf);
  if (len >= Slots) {
    const size_t start = len - Slots;
    for (size_t i = 0; i < Slots; ++i) digits[i] = buf[start + i];
    return;
  }
  const size_t pad = Slots - len;
  for (size_t i = pad; i < Slots; ++i) digits[i] = buf[i - pad];
}

// Compute the per-panel "needs refresh" mask for a fee-rate transition.
// `full_refresh` forces all slots to true. Otherwise only the digit
// positions whose glyph changed are flagged.
template <size_t Slots>
inline std::array<bool, Slots> DiffFeeRateDigits(
    const std::array<char, Slots>& now,
    const std::array<char, Slots>& before,
    bool full_refresh) {
  std::array<bool, Slots> update{};
  for (size_t i = 0; i < Slots; ++i) {
    update[i] = full_refresh || now[i] != before[i];
  }
  return update;
}

}  // namespace btclock
