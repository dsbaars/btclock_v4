// Pure-logic layout helper for the BTC-price screen.
//
// Kept in a header so the host-side doctest (and panel_texts.cpp, which
// mirrors the on-panel text to the WebUI) can include it without pulling
// in ESP-IDF headers. The matching renderer lives in btc_price.cpp and
// delegates digit positioning to `LayoutBtcPrice`.
//
// Parity note (old firmware, `parsePriceData` in lib/btclock/data_handler.cpp):
// the Arduino firmware always passed an integer price into parsePriceData
// — sub-dollar / sub-$100k decimal precision was only reachable via
// `useSuffixFormat` (which packs "78.6K" through `formatNumberWithSuffix`).
// The work tracked here adds native decimal
// precision to the non-suffix path so altcoin-like prices don't round to
// "$0" or display as "$1" for a $1.23 asset. The dedicated `.` panel
// mirrors the old firmware's `shareDot=false` suffix layout, so WebUI and
// EPD rendering stay aligned with the one existing decimal-capable path.
//
// Layout rules (7-panel → 6 digit slots; 8-panel → 7 digit slots)
// ----------------------------------------------------------------
//   Panel 0 = "BTC/<CCY>" label (unchanged).
//   Panels 1..N-1 = one character per slot, right-justified. The UTF-8
//   currency glyph (if available) lives in the first non-blank slot when
//   `use_symbol` is true *and* inserting it doesn't push a digit off the
//   panel. The `.` (when present) occupies its own dedicated slot. Digits
//   and '.' are visually positioned using `kDigitRef` — the dot's descent
//   is tiny on Antonio and the centering already places the dot near the
//   digits' baseline, so no dot-inclusive ref is required here. (Do NOT
//   widen `kDigitRef`; see test_host/test_screen_ref_chars.cpp for the
//   2026-04-23 comma-descender regression that pinned this.)
//
// Decimal count by magnitude (picked to maximize precision that still
// fits the budget, mirroring formatNumberWithSuffix's decimal-pack loop):
//
//   price >= 100000        → 0 decimals (integer; current behaviour).
//   100    <= price < 100000 → 1 decimal when the dot+digit fit.
//   1      <= price < 100   → 2 decimals when the dot+digits fit.
//   0.01   <= price < 1     → 3 decimals (sub-dollar regime — why the
//                             feature exists; altcoin-scale tickers).
//
// When the chosen decimal count doesn't fit *with* the currency glyph we
// drop the glyph before we drop decimals — keeping the displayed number's
// precision is more load-bearing than the glyph (the label already
// identifies the currency via "BTC/<CCY>"). If it still doesn't fit (e.g.
// price == 999999 on a 6-slot board), we fall back to integer truncation,
// matching the old firmware's overflow-drops-label behaviour.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace btclock {

// Dot-inclusive ref string for price digit panels that may render '.'.
// Scoped to the price renderer — do NOT move into common.hpp / widen
// `kDigitRef`. Widening the shared ref with punctuation would lower
// every digit screen's baseline uniformly (the 2026-04-23 comma-
// descender bug, pinned by test_host/test_screen_ref_chars.cpp). Kept
// in this header rather than inline in btc_price.cpp so the same
// "no raw '0123456789.' literal in a .cpp renderer" catch-all test can
// continue to guard accidental reintroduction of the shared-ref widening.
inline constexpr const char* kPriceDotRef = "0123456789.";

// Maximum decimal places for a given integer-part value. The thresholds
// are chosen so at least the "integer + '.' + decimals" width fits in a
// 6-slot (7-panel) digit region. An 8-panel board has an extra slot; it
// uses this same count for fractional prices (so low-magnitude metals /
// sub-dollar pairs keep their precision) and only suppresses decimals for
// whole-number prices — see the Slots>=7 guard in LayoutBtcPrice.
inline constexpr int PriceDecimalPlaces(double price) {
  if (!(price > 0.0)) return 0;
  if (price >= 100000.0) return 0;
  if (price >= 100.0) return 1;
  if (price >= 1.0) return 2;
  if (price >= 0.01) return 3;
  // Below 1 cent the sat-scale is too fine for this display; fall back
  // to integer (which rounds to "0") rather than emit "0.000". The data
  // source is expected to reject prices this small anyway.
  return 0;
}

// Right-justify a BTC-price render across `Slots` char cells, with an
// optional currency glyph placed one slot before the first non-blank
// character. Glyph goes into its own cell; '.' (when emitted) goes into
// its own cell as well.
//
// `use_symbol` — caller asserts that a UTF-8 glyph exists for the
// currency; the returned is_sym[k]=true marks the cell that should be
// painted with the glyph instead of a literal character.
//
// Returns true on success (values populated). Returns false only on
// invalid input (NaN / negative) and leaves all cells as ' ' in that
// case.
template <std::size_t Slots>
inline bool LayoutBtcPrice(double price, bool use_symbol,
                           std::array<char, Slots>& digits,
                           std::array<bool, Slots>& is_sym) {
  for (std::size_t i = 0; i < Slots; ++i) {
    digits[i] = ' ';
    is_sym[i] = false;
  }
  if (!(price >= 0.0)) return false;

  // Decimal count.
  //
  // 7-panel boards (Slots<7) always take the magnitude-based count: the
  // 6-slot digit region can't hold a glyph + full integer for big prices
  // anyway, and sub-dollar precision is the whole reason this path exists.
  //
  // 8-panel boards (Slots>=7) historically rendered integer-only: V8 has
  // room for the glyph *and* the full integer, and reusing the decimal
  // path produced a ". 0" tail on whole-number fiat prices (e.g. 7858 →
  // "$7858.0" spilling artefacts across the spare cells). We keep that
  // clean integer render for whole-number prices, but DO emit decimals
  // when the value has a real fractional part — low-magnitude pairs like
  // gold (~16 BTC/XAU) or a sub-dollar altcoin would otherwise lose all
  // sub-unit precision to integer rounding on the wider board. The wire
  // value drives it: fiat arrives whole, the demand-activated metals
  // arrive with decimals (AutoDecimals in the ws-node aggregator), so
  // this only widens precision where it actually exists.
  // See btc_price.cpp / parsePriceData parity in the v3 firmware.
  int decimals = PriceDecimalPlaces(price);
  if (Slots >= 7 && price == std::floor(price)) {
    decimals = 0;
  }

  // Build the textual form once, with rounding baked in. snprintf's %.*f
  // rounds half-to-even on most libcs; that matches the behaviour the
  // user sees on the already-rounded integer path.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, price);
  std::size_t len = std::strlen(buf);

  // If decimals were chosen but rounding produced a no-fractional-part
  // result (e.g. 99.999 with 2 decimals → "100.00"), the string is still
  // a valid decimal form; no need to strip the trailing zeros — having
  // the ".00" visible signals "this is a precision-aware render" and
  // matches how fee_rate.cpp presents integer-valued fractional inputs.

  // Pick between "with glyph" (budget = Slots-1) and "without glyph".
  // Prefer keeping the glyph if both fit; drop the glyph before we drop
  // decimals. If even the integer-only form doesn't fit, fall back to
  // the old firmware's overflow behaviour: truncate the *leading* digits
  // (keep the low-order magnitude), no glyph.
  bool emit_symbol = false;
  if (use_symbol && len + 1 <= Slots) {
    emit_symbol = true;
  } else if (len <= Slots) {
    emit_symbol = false;
  } else {
    // Too wide with chosen decimals — try integer-only (no dot).
    std::snprintf(buf, sizeof(buf), "%.0f", price);
    len = std::strlen(buf);
    if (use_symbol && len + 1 <= Slots) {
      emit_symbol = true;
    } else if (len > Slots) {
      // Still too wide (price with 7+ integer digits on a 6-slot board).
      // Matches parsePriceData's "integer >= NUM_SCREENS chars" branch:
      // drop the glyph, one char per slot, truncating from the left.
      const std::size_t start = len - Slots;
      for (std::size_t i = 0; i < Slots; ++i) digits[i] = buf[start + i];
      return true;
    }
  }

  // Right-justify `buf` into the cell array, reserving one leading slot
  // for the glyph when `emit_symbol` is true.
  const std::size_t content_slots = emit_symbol ? (Slots - 1) : Slots;
  const std::size_t pad = content_slots - len;
  const std::size_t dst_base = emit_symbol ? 1 : 0;
  for (std::size_t i = 0; i < len; ++i) {
    digits[dst_base + pad + i] = buf[i];
  }
  if (emit_symbol) {
    // Glyph sits immediately before the first character of the number.
    is_sym[pad] = true;
  }
  return true;
}

// Single-char-per-slot expansion for the WebUI panel-text builder. Same
// layout as `LayoutBtcPrice` but returns the per-cell string (empty for
// blanks, the UTF-8 glyph for the symbol cell, or a 1-char string for
// each digit / '.'). Kept inline so panel_texts.cpp doesn't need to
// duplicate the fit logic.
template <std::size_t Slots>
inline std::array<std::string, Slots> LayoutBtcPriceStrings(
    double price, const char* symbol_utf8) {
  std::array<char, Slots> d;
  std::array<bool, Slots> s;
  const bool use_symbol = symbol_utf8 != nullptr && symbol_utf8[0] != '\0';
  LayoutBtcPrice<Slots>(price, use_symbol, d, s);
  std::array<std::string, Slots> out;
  for (std::size_t i = 0; i < Slots; ++i) {
    if (s[i]) {
      out[i] = symbol_utf8;
    } else if (d[i] != ' ') {
      out[i].assign(1, d[i]);
    } else {
      out[i].clear();
    }
  }
  return out;
}

}  // namespace btclock
