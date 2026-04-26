// Pure-logic layout helper for the block-fee-rate screen.
//
// Kept in a header (not common.hpp) so the host-side doctest can include
// it without pulling in ESP-IDF headers. The matching renderer lives in
// fee_rate.cpp and delegates the digit positioning to `LayoutFeeRate`.
//
// Parity note (old firmware, `parseBlockFees` in lib/btclock/data_handler.cpp):
//   - Panel 0: "FEE/RATE" label
//   - Panels 1..N-2: fee digits (decimal when fractional fits; integer
//     otherwise), right-justified, ' ' for blanks
//   - Panel N-1: "sat/vB" unit text
//
// Formatting rules (decide + pin — match what you'll photograph)
// --------------------------------------------------------------
// Input is a `double` — the data hub populates `block_fee_precise` from
// btclock-ws-nostr-publish's `blockfee2` subscription (and the Nostr
// d=medianFee event, in the relay path). Special case `fee < 0` = "no
// value yet" → all digit slots blank (do NOT clamp to 0, that would lie
// about the data state).
//
// * Integer-valued fee (e.g. 42.0, 0.0, -0.0) → render as right-
//   justified integer, no dot. Keeps the visual identical to the
//   integer-only bring-up for whole-number values, which is what the
//   old-firmware parity covers too.
// * Fractional fee (e.g. 12.75, 100.5, 999.99) → format as "X.YY"
//   (integer + dot + two decimals, rounded). Right-justified into the
//   digit slots.
// * Overflow (value wider than the available slots — e.g. 1234.56 on the
//   5-slot 7-panel layout): drop the decimals first (render integer
//   only, right-justified), then truncate leading integer digits if it
//   still doesn't fit. Dropping the fractional part costs ≤ 1 sat/vB of
//   precision; dropping leading integer digits lies about the magnitude,
//   which is why it's the last resort.
//
// The layout helper writes a char buffer; the renderer turns characters
// into glyph panels. Host-test coverage lives in test_host/test_fee_rate.
//
// Ref-string note: digit panels that may contain '.' need a ref string
// that *includes* the dot so `Font::GetReferenceBox` returns a baseline
// consistent with the digits. `kDigitRef` ("0123456789" in common.hpp)
// must not be widened with punctuation — the 2026-04-23 comma-descender
// bug was exactly that kind of silent drift across screens. The local
// `kFeeRateDotRef` below is scoped to this screen only and is used by
// fee_rate.cpp for the dot-containing digit panels; all other screens
// continue to use `kDigitRef`. See test_host/test_screen_ref_chars.cpp
// for the regression.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace btclock {

// Number of digit panels consumed by the fee-rate layout. Panel 0 is the
// label, panel N-1 is the "sat/vB" unit, panels 1..N-2 are digits.
template <size_t N>
inline constexpr size_t kFeeRateDigitPanels = N - 2;

// Dot-inclusive ref string for the fee-rate digit panels. Scoped to the
// fee-rate renderer — do NOT move into common.hpp / kDigitRef. Widening
// the shared ref with punctuation would drop every digit screen's
// baseline by ~11 px (Antonio's dot has a small descender; any comma /
// colon would be much worse). See the comment in common.hpp and the
// regression test in test_host/test_screen_ref_chars.cpp.
inline constexpr const char* kFeeRateDotRef = "0123456789.";

// Right-justify the formatted form of `fee_sats_vb` into `digits`.
// Leading positions get ' '. If `fee_sats_vb < 0`, all positions are
// left as ' ' (not-yet-received state).
//
// Integer-valued doubles render without a dot; fractional values render
// as "X.YY" (two decimals, rounded half-away-from-zero via std::round
// on the intermediate cents value). On overflow the fractional tail is
// dropped before leading integer digits.
template <size_t Slots>
inline void LayoutFeeRate(double fee_sats_vb, std::array<char, Slots>& digits) {
  for (size_t i = 0; i < Slots; ++i) digits[i] = ' ';
  if (!(fee_sats_vb >= 0.0)) return;  // catches NaN + negatives

  // Round to 2 decimals first so the "integer-valued" check below is
  // based on the *displayed* rounded value, not the noisy input — e.g.
  // 41.999999 becomes 42.00 (integer-valued) rather than rendering as
  // "41.99". `std::round` gives half-away-from-zero; `%.2f` below
  // matches via the same rounded intermediate.
  const double rounded_cents = std::round(fee_sats_vb * 100.0);
  const double rounded = rounded_cents / 100.0;
  const long long cents_int = static_cast<long long>(rounded_cents);
  const bool integer_valued = (cents_int % 100) == 0;

  char buf[32];
  if (integer_valued) {
    // Plain integer render.
    const long long iv = static_cast<long long>(std::llround(rounded));
    std::snprintf(buf, sizeof(buf), "%lld", iv);
  } else {
    // Decimal with 2 places.
    std::snprintf(buf, sizeof(buf), "%.2f", rounded);
  }

  size_t len = std::strlen(buf);
  if (len > Slots) {
    // Too wide. Try dropping decimals first (integer-only fallback).
    const long long iv = static_cast<long long>(std::llround(rounded));
    std::snprintf(buf, sizeof(buf), "%lld", iv);
    len = std::strlen(buf);
    if (len > Slots) {
      // Still too wide — truncate from the left (last-resort).
      const size_t start = len - Slots;
      for (size_t i = 0; i < Slots; ++i) digits[i] = buf[start + i];
      return;
    }
  }
  const size_t pad = Slots - len;
  for (size_t i = pad; i < Slots; ++i) digits[i] = buf[i - pad];
}

// Compute the per-panel "needs refresh" mask for a fee-rate transition.
// `full_refresh` forces all slots to true. Otherwise only the digit
// positions whose glyph changed are flagged.
template <size_t Slots>
inline std::array<bool, Slots> DiffFeeRateDigits(
    const std::array<char, Slots>& now, const std::array<char, Slots>& before,
    bool full_refresh) {
  std::array<bool, Slots> update{};
  for (size_t i = 0; i < Slots; ++i) {
    update[i] = full_refresh || now[i] != before[i];
  }
  return update;
}

}  // namespace btclock
