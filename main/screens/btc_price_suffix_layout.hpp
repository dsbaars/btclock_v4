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

#include "screens/screen_math.hpp"

namespace btclock {

template <std::size_t Panels>
inline std::array<std::string, Panels> LayoutBtcPriceSuffixStrings(
    std::uint64_t price_int, const std::string& currency,
    const char* symbol_utf8, bool mow_mode, bool share_dot,
    std::string& out_label) {
  // share_dot bumps num_chars to Panels-1 (one more digit cell) and
  // folds the '.' byte into the cell immediately before it. mow_mode
  // already runs at Panels-1 because its M-suffix layout needs the
  // extra width regardless; v3 parsePriceData treats the bump as
  // either/or rather than additive. The fold applies to both modes —
  // see test_datahandler_parity PriceSuffixModeMowCompact.
  const int num_chars = (mow_mode || share_dot) ? static_cast<int>(Panels) - 1
                                                : static_cast<int>(Panels) - 2;
  const std::string num_str =
      FormatNumberWithSuffix(price_int, num_chars, mow_mode);
  const bool has_symbol = symbol_utf8 != nullptr && symbol_utf8[0] != '\0';
  // Treat the currency glyph as one cell regardless of its UTF-8 byte
  // length. v3 composes "$" + num_str (single byte) and pads against
  // NUM_SCREENS; pad at cell granularity here so multi-byte glyphs
  // (€, £, ¥) stay in their own cell.
  const std::size_t raw_cells_len = (has_symbol ? 1u : 0u) + num_str.size();
  // Compute the fold up front: the visual width drives the label-path
  // guard, not the raw byte count. Without this the label path bails
  // for cases like 78080 + share_dot (raw=7, visual=6) on a 7-panel.
  const std::size_t dot_pos = share_dot ? num_str.find('.') : std::string::npos;
  const std::size_t fold_savings =
      (dot_pos != std::string::npos && dot_pos > 0) ? 1u : 0u;
  const std::size_t cells_len = raw_cells_len - fold_savings;

  std::array<std::string, Panels> out;
  for (auto& s : out) s.clear();

  if (cells_len < Panels) {
    // Label path. Panel 0 stays empty in `out`; caller paints label.
    // v3 swapped to "MOW/UNITS" on mow_mode; v4 keeps "BTC/<CCY>"
    // regardless so the currency stays visible on the M-suffix form.
    out_label = std::string("BTC/") + currency;
    const std::size_t digit_cells = Panels - 1;
    const std::size_t pad =
        cells_len < digit_cells ? digit_cells - cells_len : 0u;
    std::size_t idx = 1 + pad;  // +1 skips panel 0 (label slot)
    if (has_symbol) {
      out[idx++] = symbol_utf8;
    }
    for (std::size_t i = 0; i < num_str.size() && idx < Panels; ++i) {
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

  // Overflow path: priceString fills (or exceeds) all Panels cells.
  // v3 drops the label and emits one char per panel from the head of
  // priceString. If priceString exceeds Panels the tail is silently
  // truncated, matching v3's `ret[i] = priceString[i]` loop that only
  // writes NUM_SCREENS cells. share_dot does not apply here — the
  // overflow path is char-per-cell by definition.
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
