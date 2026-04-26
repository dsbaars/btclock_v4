#include "screens/screens.hpp"

#include <array>
#include <cstdlib>
#include <string>

#include "screens/btc_price_suffix_layout.hpp"
#include "screens/common.hpp"
#include "screens/price_layout.hpp"

namespace btclock {

namespace {

// Parse the raw price string into a double. Returns -1.0 on parse
// failure so callers can blank the digit area. Mirrors the guards in
// `PriceInt` (common.cpp) but keeps the fractional part.
double ParsePriceDouble(const std::string& s) {
  if (s.empty()) return -1.0;
  char* endp = nullptr;
  const double p = std::strtod(s.c_str(), &endp);
  if (endp == s.c_str()) return -1.0;
  if (!(p >= 0.0)) return -1.0;
  // Same upper bound as PriceInt — keeps multiply overflow in
  // market-cap math bounded (supply * price ≤ 2e9 * 21e6 ≈ 4.2e16).
  if (p > 2e9) return -1.0;
  return p;
}

// Integer parse for the suffix/MOW path (FormatNumberWithSuffix takes
// uint64). Rounds half-away-from-zero to match v3 parsePriceData which
// received a `uint32_t price` upstream. Negative / parse-error → -1.
int64_t ParsePriceInt(const std::string& s) {
  const double d = ParsePriceDouble(s);
  if (d < 0.0) return -1;
  return static_cast<int64_t>(d + 0.5);
}

// Decide whether the renderer takes the suffix/MOW path. Matches v3
// parsePriceData's `std::to_string(price).length() >= NUM_SCREENS ||
// useSuffixFormat` guard: either the user opted in explicitly, or the
// integer price has too many digits to fit the plain path.
template <std::size_t N>
bool ShouldUseSuffixPath(int64_t price_int, bool suffix_price) {
  if (price_int < 0) return false;
  if (suffix_price) return true;
  return std::to_string(price_int).size() >= N;
}

}  // namespace

// Panel 0 = "BTC/<CCY>" label (or "MOW/UNITS" on the suffix+mow fits-
// with-label path; blank on the suffix overflow path where priceString
// fills all panels). Panels 1..N-1 = one character per slot, right-
// justified. Layout is computed by `LayoutBtcPrice` (plain path) or
// `LayoutBtcPriceSuffixStrings` (suffix / MOW path); both are pure-
// logic helpers shared with panel_texts.cpp for WebUI parity.
//
// Partial refresh: compare the per-panel text+is-symbol tuple to the
// previous frame and repaint only the cells that changed.

template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price, const char* symbol_utf8,
    bool suffix_price, bool mow_mode, bool share_dot,
    bool full_refresh_mode, bool vertical_desc) {
  static_assert(N >= 7, "Price layout needs at least 7 panels");
  // `cell_diff_reset` forces every cell to repaint (sentinel prev_price);
  // `full_refresh_mode` drives the EPD refresh kind. See screens.hpp.
  const bool cell_diff_reset = prev_price.empty();
  const bool use_symbol = symbol_utf8 && symbol_utf8[0] != '\0';

  // Per-panel cell-string + is-symbol flags for the new and (for diff)
  // old frames. Both layout paths are normalised into this shared
  // representation so the paint loop below stays uniform.
  std::array<std::string, N> new_cells;
  std::array<std::string, N> old_cells;
  std::array<bool, N> new_is_sym;
  std::array<bool, N> old_is_sym;
  for (size_t i = 0; i < N; ++i) {
    new_cells[i].clear();
    old_cells[i].clear();
    new_is_sym[i] = false;
    old_is_sym[i] = false;
  }
  // Label override — when non-empty, panel 0 is painted as a
  // split-text label instead of the cell string. The suffix path
  // leaves this empty on the overflow branch (cell 0 carries a
  // priceString char and the label is dropped).
  std::string new_label = std::string("BTC/") + currency;
  std::string old_label = new_label;

  auto fill_suffix = [&](uint64_t pi,
                         std::array<std::string, N>& cells_out,
                         std::array<bool, N>& sym_out,
                         std::string& label_out) {
    std::string lbl;
    auto cells = LayoutBtcPriceSuffixStrings<N>(pi, currency, symbol_utf8,
                                                mow_mode, share_dot, lbl);
    label_out = lbl;
    for (size_t i = 0; i < N; ++i) {
      cells_out[i] = cells[i];
      sym_out[i] = use_symbol && !cells[i].empty() &&
                   cells[i] == std::string(symbol_utf8);
    }
  };

  auto fill_plain = [&](const std::string& p,
                        std::array<std::string, N>& cells_out,
                        std::array<bool, N>& sym_out) {
    constexpr size_t kDigitPanels = N - 1;
    std::array<char, kDigitPanels> d;
    std::array<bool, kDigitPanels> s;
    LayoutBtcPrice<kDigitPanels>(ParsePriceDouble(p), use_symbol, d, s);
    // Cell 0 is the label slot — stays empty; caller paints label.
    cells_out[0].clear();
    sym_out[0] = false;
    for (size_t i = 0; i < kDigitPanels; ++i) {
      if (s[i]) {
        cells_out[i + 1] = symbol_utf8;
        sym_out[i + 1] = true;
      } else if (d[i] != ' ') {
        cells_out[i + 1].assign(1, d[i]);
        sym_out[i + 1] = false;
      } else {
        cells_out[i + 1].clear();
        sym_out[i + 1] = false;
      }
    }
  };

  const int64_t new_int = ParsePriceInt(price);
  const bool new_suffix = ShouldUseSuffixPath<N>(new_int, suffix_price);
  if (new_suffix) {
    fill_suffix(static_cast<uint64_t>(new_int < 0 ? 0 : new_int),
                new_cells, new_is_sym, new_label);
  } else {
    fill_plain(price, new_cells, new_is_sym);
  }

  if (!cell_diff_reset) {
    const int64_t prev_int = ParsePriceInt(prev_price);
    const bool prev_suffix =
        ShouldUseSuffixPath<N>(prev_int, suffix_price);
    if (prev_suffix) {
      fill_suffix(static_cast<uint64_t>(prev_int < 0 ? 0 : prev_int),
                  old_cells, old_is_sym, old_label);
    } else {
      fill_plain(prev_price, old_cells, old_is_sym);
    }
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — the label (non-empty → paint as kLabelSplit) or the
  // overflow-cell character (suffix path drops the label when
  // priceString fills all panels). Repaint when the text/kind changes.
  if (!new_label.empty()) {
    slots[0] = PaintSlot{PaintSlot::kLabelSplit, new_label, nullptr, 0, 0};
  } else if (new_is_sym[0]) {
    slots[0] = PaintSlot{PaintSlot::kCurrencyGlyph, new_cells[0],
                         nullptr, 0, 0};
  } else if (!new_cells[0].empty()) {
    slots[0] = PaintSlot{PaintSlot::kDigit, new_cells[0], nullptr, 0, 0};
  } else {
    slots[0] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
  }
  update[0] = cell_diff_reset || full_refresh_mode ||
              new_label != old_label ||
              new_cells[0] != old_cells[0] ||
              new_is_sym[0] != old_is_sym[0];

  // Digit / currency-glyph cells. Blank cells (empty string) stay blank
  // — kBlank on partial refresh is a no-op. Digit cells here are single
  // ASCII chars (the '.' cell is one byte); the suffix path may emit a
  // single UTF-8 currency glyph in its symbol cell which we route
  // through kCurrencyGlyph.
  for (size_t i = 1; i < N; ++i) {
    if (new_is_sym[i]) {
      slots[i] = PaintSlot{PaintSlot::kCurrencyGlyph, new_cells[i],
                           nullptr, 0, 0};
    } else if (new_cells[i].empty()) {
      slots[i] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
    } else {
      slots[i] = PaintSlot{PaintSlot::kDigit, new_cells[i],
                           nullptr, 0, 0};
    }
    update[i] = cell_diff_reset || full_refresh_mode ||
                new_cells[i] != old_cells[i] ||
                new_is_sym[i] != old_is_sym[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update,
                  full_refresh_mode, vertical_desc);
}

template void RenderBtcPriceScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*, bool, bool, bool, bool, bool);
template void RenderBtcPriceScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*, bool, bool, bool, bool, bool);

}  // namespace btclock
