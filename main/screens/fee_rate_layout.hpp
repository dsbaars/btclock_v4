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
// * Fee strictly below 10 sat/vB (after rounding) → always two decimal
//   places ("1.00", "0.00"), even when the rounded value is integer-
//   valued — matches user expectation for low-fee precision.
// * Integer-valued fee at ≥ 10 (e.g. 42.0, 10.0) → plain integer, no dot.
// * Otherwise fractional (e.g. 12.75, 100.5, 999.99) → format as "X.YY"
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
#include <string>
#include <vector>

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

// Runtime-N core of LayoutFeeRate. The distributed-display strip lays the
// fee value across the summed panel count of every peer, so the panel-text
// builder needs a fit that isn't fixed to a compile-time slot count. The
// templated LayoutFeeRate below delegates here so the on-device renderer
// and the wide-strip builder share one implementation.
inline void LayoutFeeRateRuntime(double fee_sats_vb, std::size_t slots,
                                 std::vector<char>& digits) {
  digits.assign(slots, ' ');
  if (!(fee_sats_vb >= 0.0)) return;  // catches NaN + negatives

  // Round to 2 decimals first so the "integer-valued" check is based on
  // the *displayed* rounded value, not the noisy input (41.999999 → 42.00).
  const double rounded_cents = std::round(fee_sats_vb * 100.0);
  const double rounded = rounded_cents / 100.0;
  const long long cents_int = static_cast<long long>(rounded_cents);
  const bool integer_valued = (cents_int % 100) == 0;
  const bool force_two_decimals = (rounded < 10.0);

  char buf[32];
  if (integer_valued && !force_two_decimals) {
    const long long iv = static_cast<long long>(std::llround(rounded));
    std::snprintf(buf, sizeof(buf), "%lld", iv);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f", rounded);
  }

  std::size_t len = std::strlen(buf);
  if (len > slots) {
    // Too wide. Drop decimals first (integer-only), then truncate from
    // the left as a last resort.
    const long long iv = static_cast<long long>(std::llround(rounded));
    std::snprintf(buf, sizeof(buf), "%lld", iv);
    len = std::strlen(buf);
    if (len > slots) {
      const std::size_t start = len - slots;
      for (std::size_t i = 0; i < slots; ++i) digits[i] = buf[start + i];
      return;
    }
  }
  const std::size_t pad = slots - len;
  for (std::size_t i = pad; i < slots; ++i) digits[i] = buf[i - pad];
}

// Runtime-N core of FeeRateDigitCells (see the templated version for the
// share_dot fold semantics).
inline std::vector<std::string> FeeRateDigitCellsRuntime(
    const std::vector<char>& digits, bool share_dot) {
  const std::size_t slots = digits.size();
  std::vector<std::string> cells(slots);
  for (std::size_t i = 0; i < slots; ++i) {
    const char c = digits[i];
    cells[i] = (c == ' ') ? std::string() : std::string(1, c);
  }
  if (!share_dot) return cells;

  std::size_t dot_idx = slots;
  for (std::size_t i = 0; i < slots; ++i) {
    if (cells[i] == ".") {
      dot_idx = i;
      break;
    }
  }
  if (dot_idx >= slots) return cells;

  std::size_t run_start = dot_idx;
  while (run_start > 0 && cells[run_start - 1].size() == 1 &&
         cells[run_start - 1][0] >= '0' && cells[run_start - 1][0] <= '9') {
    --run_start;
  }
  if (run_start == dot_idx) return cells;

  std::string merged;
  for (std::size_t k = run_start; k < dot_idx; ++k) merged += cells[k];
  merged += '.';

  std::vector<std::string> seq_build;
  seq_build.reserve(slots);
  for (std::size_t k = 0; k < run_start; ++k) {
    if (!cells[k].empty()) seq_build.push_back(std::move(cells[k]));
  }
  seq_build.push_back(std::move(merged));
  for (std::size_t k = dot_idx + 1; k < slots; ++k) {
    if (!cells[k].empty()) seq_build.push_back(std::move(cells[k]));
  }

  std::vector<std::string> out(slots);
  const std::size_t n = seq_build.size();
  const std::size_t lead = (slots >= n) ? slots - n : 0;
  for (std::size_t i = 0; i < n && lead + i < slots; ++i) {
    out[lead + i] = std::move(seq_build[i]);
  }
  return out;
}

// Right-justify the formatted form of `fee_sats_vb` into `digits`.
// Leading positions get ' '. If `fee_sats_vb < 0`, all positions are
// left as ' ' (not-yet-received state). Fixed-N façade over
// LayoutFeeRateRuntime for the on-device renderer (fee_rate.cpp).
//
// Values below 10 always use two decimals; 10+ integer-valued doubles use
// no dot; other fractional values use "X.YY" (rounded half-away-from-zero
// via std::round on the cents). On overflow the fractional tail is dropped
// before leading integer digits.
template <size_t Slots>
inline void LayoutFeeRate(double fee_sats_vb, std::array<char, Slots>& digits) {
  std::vector<char> d;
  LayoutFeeRateRuntime(fee_sats_vb, Slots, d);
  for (size_t i = 0; i < Slots; ++i) digits[i] = d[i];
}

// `decimalShareDot` path: merge the '.' into one cell with every digit
// that precedes it ("12." not "2."), then right-pack so blanks stay on
// the FEE/RATE side — no empty panel between the last fee digit and
// "sat/vB". No-op when share_dot is false; when true but there is no '.',
// returns the per-char cells unchanged.
template <size_t Slots>
inline std::array<std::string, Slots> FeeRateDigitCells(
    const std::array<char, Slots>& digits, bool share_dot) {
  std::vector<char> d(digits.begin(), digits.end());
  const std::vector<std::string> r = FeeRateDigitCellsRuntime(d, share_dot);
  std::array<std::string, Slots> out{};
  for (size_t i = 0; i < Slots && i < r.size(); ++i) out[i] = r[i];
  return out;
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

template <size_t Slots>
inline std::array<bool, Slots> DiffFeeRateCells(
    const std::array<std::string, Slots>& now,
    const std::array<std::string, Slots>& before, bool full_refresh) {
  std::array<bool, Slots> update{};
  for (size_t i = 0; i < Slots; ++i) {
    update[i] = full_refresh || now[i] != before[i];
  }
  return update;
}

}  // namespace btclock
