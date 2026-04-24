// Pure-logic layout helper for the BTC-price suffix / MOW modes.
//
// Ports v3 parsePriceData's useSuffixFormat + mowMode branches from
// btclock_v3_fci lib/btclock/data_handler.cpp. Kept in its own header
// (vs. living in price_layout.hpp) so the renderer and panel_texts
// mirror share one implementation without touching the existing v4
// sub-dollar decimal-precision path.
//
// Rules (byte-for-byte from v3 with shareDot=false):
//   numChars := mow_mode ? Panels-1 : Panels-2       (NUM_SCREENS-1 or -2)
//   priceString := glyph + FormatNumberWithSuffix(price, numChars, mow)
//   if priceString.size() < Panels   → label path:
//     out_label = mow_mode ? "MOW/UNITS" : "BTC/<CCY>"
//     cells[0] is left empty (caller paints out_label on panel 0)
//     cells[1..Panels-1] hold the priceString left-padded in Panels-1
//       cells (leading spaces → empty strings).
//   else                              → overflow path:
//     out_label is cleared
//     cells[0..Panels-1] hold priceString char-per-cell (label dropped).
//
// `mow_mode=true` forces the M-suffix form via FormatNumberWithSuffix's
// mow branch: 78280 → "0.078M", 1_000_000 → "1.000M". v3 passes mowMode
// through unconditionally whenever the suffix branch fires; `suffix_price`
// is what gates the branch (vs. the integer LayoutBtcPrice path).
// Precedence on !suffix_price+mow_mode short prices: v3 ignores mow —
// parsePriceData only enters the suffix branch when useSuffixFormat is
// set or the integer itself overflows — callers should gate accordingly.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "screens/screen_math.hpp"

namespace btclock {

template <std::size_t Panels>
inline std::array<std::string, Panels> LayoutBtcPriceSuffixStrings(
    std::uint64_t price_int, const std::string& currency,
    const char* symbol_utf8, bool mow_mode, std::string& out_label) {
  const int num_chars = mow_mode
                            ? static_cast<int>(Panels) - 1
                            : static_cast<int>(Panels) - 2;
  const std::string num_str =
      FormatNumberWithSuffix(price_int, num_chars, mow_mode);
  const bool has_symbol = symbol_utf8 != nullptr && symbol_utf8[0] != '\0';
  // Treat the currency glyph as one cell regardless of its UTF-8 byte
  // length. v3 composes "$" + num_str (single byte) and pads against
  // NUM_SCREENS; pad at cell granularity here so multi-byte glyphs
  // (€, £, ¥) stay in their own cell.
  const std::size_t cells_len = (has_symbol ? 1u : 0u) + num_str.size();

  std::array<std::string, Panels> out;
  for (auto& s : out) s.clear();

  if (cells_len < Panels) {
    // Label path. Panel 0 stays empty in `out`; caller paints label.
    out_label = mow_mode ? std::string("MOW/UNITS")
                         : (std::string("BTC/") + currency);
    const std::size_t digit_cells = Panels - 1;
    const std::size_t pad = digit_cells - cells_len;
    std::size_t idx = 1 + pad;  // +1 skips panel 0 (label slot)
    if (has_symbol) {
      out[idx++] = symbol_utf8;
    }
    for (std::size_t i = 0; i < num_str.size() && idx < Panels;
         ++i, ++idx) {
      out[idx].assign(1, num_str[i]);
    }
    return out;
  }

  // Overflow path: priceString fills (or exceeds) all Panels cells.
  // v3 drops the label and emits one char per panel from the head of
  // priceString. If priceString exceeds Panels the tail is silently
  // truncated, matching v3's `ret[i] = priceString[i]` loop that only
  // writes NUM_SCREENS cells.
  out_label.clear();
  std::size_t idx = 0;
  if (has_symbol && idx < Panels) {
    out[idx++] = symbol_utf8;
  }
  for (std::size_t i = 0; i < num_str.size() && idx < Panels; ++i, ++idx) {
    out[idx].assign(1, num_str[i]);
  }
  return out;
}

}  // namespace btclock
