// Pure-logic layout helper for the BTC-price suffix / MOW modes.
//
// Ports v3 parsePriceData's useSuffixFormat + mowMode branches from
// the v3 firmware's lib/btclock/data_handler.cpp. Kept in its own header
// (vs. living in price_layout.hpp) so the renderer and panel_texts
// mirror share one implementation without touching the existing v4
// sub-dollar decimal-precision path.
//
// Rules (byte-for-byte from v3):
//   numChars := (mow_mode || share_dot) ? Panels-1 : Panels-2
//   priceString := glyph + FormatNumberWithSuffix(price, numChars, mow)
//   share_dot folds the '.' byte into its preceding cell ("X."), so
//   the visual width is `priceString.size() - (has_dot ? 1 : 0)`.
//   if visual width <= Panels-1   → label path:
//     out_label = "BTC/<CCY>"  (v3 emitted "MOW/UNITS" when mow_mode
//       was set; v4 keeps the BTC/<CCY> label so currency context is
//       not lost on the MOW form.)
//     cells[0] is left empty (caller paints out_label on panel 0)
//     cells[1..Panels-1] hold the priceString left-padded in Panels-1
//       cells (leading spaces → empty strings; '.' folded when set).
//   else                          → overflow path:
//     out_label is cleared
//     cells[0..Panels-1] hold priceString char-per-cell (label dropped,
//       fold disabled — overflow is char-per-cell by definition).
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
#include <vector>

#include "screens/screen_math.hpp"

namespace btclock {

// Runtime-N core of LayoutBtcPriceSuffixStrings. The distributed-display
// strip lays the suffix/MOW form across the summed panel count of every
// peer, so the panel-text builder needs a layout that isn't fixed to a
// compile-time panel count. The templated version below delegates here so
// the on-device renderer and the wide-strip builder share one
// implementation — parity at N=7/8 is pinned by the host tests.
inline std::vector<std::string> LayoutBtcPriceSuffixStringsRuntime(
    std::uint64_t price_int, const std::string& currency,
    const char* symbol_utf8, std::size_t panels, bool mow_mode, bool share_dot,
    std::string& out_label) {
  // share_dot bumps num_chars to panels-1 (one more digit cell) and folds
  // the '.' byte into the cell immediately before it. mow_mode already
  // runs at panels-1 because its M-suffix layout needs the extra width;
  // v3 parsePriceData treats the bump as either/or rather than additive.
  const int num_chars = (mow_mode || share_dot) ? static_cast<int>(panels) - 1
                                                : static_cast<int>(panels) - 2;
  const std::string num_str =
      FormatNumberWithSuffix(price_int, num_chars, mow_mode);
  const bool has_symbol = symbol_utf8 != nullptr && symbol_utf8[0] != '\0';
  // Treat the currency glyph as one cell regardless of its UTF-8 byte
  // length so multi-byte glyphs (€, £, ¥) stay in their own cell.
  const std::size_t raw_cells_len = (has_symbol ? 1u : 0u) + num_str.size();
  const std::size_t dot_pos = share_dot ? num_str.find('.') : std::string::npos;
  const std::size_t fold_savings =
      (dot_pos != std::string::npos && dot_pos > 0) ? 1u : 0u;
  const std::size_t cells_len = raw_cells_len - fold_savings;

  std::vector<std::string> out(panels);

  if (cells_len < panels) {
    // Label path. Panel 0 stays empty in `out`; caller paints label.
    out_label = std::string("BTC/") + currency;
    const std::size_t digit_cells = panels - 1;
    const std::size_t pad =
        cells_len < digit_cells ? digit_cells - cells_len : 0u;
    std::size_t idx = 1 + pad;  // +1 skips panel 0 (label slot)
    if (has_symbol) {
      out[idx++] = symbol_utf8;
    }
    for (std::size_t i = 0; i < num_str.size() && idx < panels; ++i) {
      if (fold_savings && i + 1 == dot_pos) {
        // Pack "X." into one cell, skip the raw dot byte.
        out[idx++] = std::string(1, num_str[i]) + ".";
        ++i;
      } else {
        out[idx++].assign(1, num_str[i]);
      }
    }
    return out;
  }

  // Overflow path: priceString fills (or exceeds) all `panels` cells.
  // Drop the label, one char per panel from the head; a longer string is
  // silently truncated. share_dot does not apply here.
  out_label.clear();
  std::size_t idx = 0;
  if (has_symbol && idx < panels) {
    out[idx++] = symbol_utf8;
  }
  for (std::size_t i = 0; i < num_str.size() && idx < panels; ++i, ++idx) {
    out[idx].assign(1, num_str[i]);
  }
  return out;
}

// Fixed-N façade over LayoutBtcPriceSuffixStringsRuntime for the on-device
// renderer (btc_price.cpp), which consumes a std::array.
template <std::size_t Panels>
inline std::array<std::string, Panels> LayoutBtcPriceSuffixStrings(
    std::uint64_t price_int, const std::string& currency,
    const char* symbol_utf8, bool mow_mode, bool share_dot,
    std::string& out_label) {
  const std::vector<std::string> r = LayoutBtcPriceSuffixStringsRuntime(
      price_int, currency, symbol_utf8, Panels, mow_mode, share_dot, out_label);
  std::array<std::string, Panels> out;
  for (std::size_t i = 0; i < Panels && i < r.size(); ++i) out[i] = r[i];
  return out;
}

}  // namespace btclock
