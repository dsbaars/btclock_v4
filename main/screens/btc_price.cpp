#include "screens/screens.hpp"

#include <array>
#include <cstdlib>

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

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    // Inherit the digit font so the WASM preview's swappable antonio
    // slot carries the label too (Bug 1 — see block_height.cpp for the
    // full note). The old oswald_bold left labels on Oswald even when
    // the user picked a different digit font.
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BTC",
                  currency.c_str(),
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.antonio(), 54.0f, /*white_text=*/false);
  }

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

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh || new_digits[i] != old_digits[i] ||
                new_sym[i] != old_sym[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (new_sym[i]) {
      // Currency glyph cell — baseline via `kDigitRef` so the glyph
      // lines up with the digits on adjacent panels.
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                       symbol_utf8, kDigitRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
    } else if (new_digits[i] == '.') {
      // Dedicated '.' cell. Using the dot-inclusive local ref keeps the
      // dot's vertical position consistent with the digits while leaving
      // the shared `kDigitRef` untouched.
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, ".",
                       kPriceDotRef, fonts.antonio(), 180.0f, false);
    } else if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      // Plain digit cell. Uses the shared digit-only ref so this
      // baseline matches block-height / Moscow-time / the symbol cell.
      // Do NOT widen this with '.' — the shared-ref widening regression
      // is covered in test_host/test_screen_ref_chars.cpp.
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kDigitRef, fonts.antonio(), 180.0f, false);
    }
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->DrawFramebufferStart(fb_storage[1 + i], kind);
  }
  if (full_refresh) panels[0]->WaitForRefresh();
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->WaitForRefresh();
  }
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
