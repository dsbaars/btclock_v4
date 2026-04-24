#include "screens/screens.hpp"

#include <array>
#include <cstdlib>
#include <string>

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

}  // namespace

// Panel 0 = "BTC/<CCY>", panels 1..N-1 = one character per slot,
// right-justified. Layout is computed by `LayoutBtcPrice`, which
// handles the integer / decimal-precision / currency-glyph-placement
// decisions in one spot (also consumed by panel_texts.cpp for WebUI
// parity). Partial refresh: compare the per-cell (char + is-symbol)
// tuple to the previous frame and repaint only the cells that changed.

template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price, const char* symbol_utf8) {
  static_assert(N >= 7, "Price layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;
  const bool full_refresh = prev_price.empty();
  const bool use_symbol = symbol_utf8 && symbol_utf8[0] != '\0';

  std::array<char, kDigitPanels> new_digits;
  std::array<char, kDigitPanels> old_digits;
  std::array<bool, kDigitPanels> new_sym;
  std::array<bool, kDigitPanels> old_sym;
  LayoutBtcPrice<kDigitPanels>(ParsePriceDouble(price), use_symbol,
                               new_digits, new_sym);
  if (!full_refresh) {
    LayoutBtcPrice<kDigitPanels>(ParsePriceDouble(prev_price), use_symbol,
                                 old_digits, old_sym);
  } else {
    for (size_t i = 0; i < kDigitPanels; ++i) {
      old_digits[i] = ' ';
      old_sym[i] = false;
    }
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — "BTC/<CCY>" label. Doesn't change across price updates
  // within a single currency, so only paint on full refresh.
  slots[0] = PaintSlot{PaintSlot::kLabelSplit,
                       std::string("BTC/") + currency, nullptr, 0, 0};
  update[0] = full_refresh;

  // Digit / currency-glyph / '.' cells. LayoutBtcPrice has already
  // emitted ' ' for leading pad positions; kDigit treats ' ' as
  // "don't paint" so leading cells stay blank after ClearFb. The '.'
  // cell is painted as a regular digit — using kDigitRef instead of
  // the old kPriceDotRef: '.' has no descenders, so the reference box
  // is byte-identical to the digit-only ref in practice, which the
  // SHA-256 hash diff verifies.
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    if (new_sym[i]) {
      slots[panel_idx] = PaintSlot{PaintSlot::kCurrencyGlyph,
                                   std::string(symbol_utf8),
                                   nullptr, 0, 0};
    } else {
      slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                   std::string(1, new_digits[i]),
                                   nullptr, 0, 0};
    }
    update[panel_idx] = full_refresh || new_digits[i] != old_digits[i] ||
                        new_sym[i] != old_sym[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderBtcPriceScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*);
template void RenderBtcPriceScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*);

}  // namespace btclock
