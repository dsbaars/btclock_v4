#include <array>
#include <string>

#include "screens/common.hpp"
#include "screens/sats_per_currency_layout.hpp"
#include "screens/screens.hpp"

namespace btclock {

// "Moscow time" = sats per currency unit, shown as the raw integer
// right-justified across the digit panels with an optional sats glyph
// placed one panel before the first digit. For runtime-fetched codes
// whose 1-unit value is too small to round to >= 1 sat (weak fiat —
// VND, IRR, LBP, …) the screen instead lays out "0.dddd" so users see
// the actual sub-sat ratio rather than "0".
//
// Digit-slot count is N-1 (all panels after the label). Bug 3 — the
// previous layout hard-coded 6 digit slots so the 8-panel V8 variant
// left panel 7 blank; the old firmware parseSatsPerCurrency instead
// uses every slot (pad + digits, STS marker just before the first
// digit), which is what V8 hardware needs to look like.

template <size_t N>
void RenderMoscowTimeScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                            uint8_t (&fb_storage)[N][16 * 296],
                            const AppFonts& fonts, const std::string& currency,
                            const std::string& price,
                            const std::string& prev_price, uint8_t sats_variant,
                            bool use_sats_symbol, bool use_mscw_time,
                            bool full_refresh_mode, bool vertical_desc) {
  static_assert(N >= 7, "Moscow-time layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;
  // `cell_diff_reset` forces every cell to repaint; `full_refresh_mode`
  // drives the EPD refresh kind. See screens.hpp for the contract.
  const bool cell_diff_reset = prev_price.empty();

  // Integer sats — kept for the "MSCW/TIME" label decision below
  // (Moscow-time only applies to the integer path, USD, and the 5-digit
  // range). The render-side digit layout uses ComputeSatsPerCurrencyLayout
  // which handles the < 1 sats-per-currency case as "0.dddd".
  const int32_t new_sats_int = SatsPerUnit(price);

  // use_sats_symbol=false feeds `use_symbol=false` into the layout so
  // the marker cell never gets flagged — the EPD paints a blank there.
  const auto now =
      ComputeSatsPerCurrencyLayout<kDigitPanels>(price, use_sats_symbol);
  const auto before = cell_diff_reset
                          ? SatsPerCurrencyLayout<kDigitPanels>{}
                          : ComputeSatsPerCurrencyLayout<kDigitPanels>(
                                prev_price, use_sats_symbol);

  const auto glyph = SatsGlyphUtf8(sats_variant);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — label. USD in the classic Moscow-time range (> 1 USD per
  // sat) gets "MSCW/TIME"; otherwise "SATS/<CCY>". useMscwTime=false
  // forces SATS/<CCY> regardless of range — matches the old firmware's
  // opt-out (some users prefer the uniform label). Label doesn't change
  // across price updates within the same range, so only paint when a
  // cell-diff reset or full EPD refresh is in play.
  const bool moscow = use_mscw_time && currency == "USD" && !now.fractional &&
                      new_sats_int > 0 && new_sats_int < 100000;
  const std::string label_text =
      moscow ? std::string("MSCW/TIME") : (std::string("SATS/") + currency);
  slots[0] = PaintSlot{PaintSlot::kLabelSplit, label_text, nullptr, 0, 0};
  update[0] = cell_diff_reset || full_refresh_mode;

  // Digit panels — right-justified across `kDigitPanels` cells starting
  // at panel 1. `is_sats[i]` flags the sats-glyph cell one slot before
  // the first digit (integer path only); paint it via kSatsGlyph at the
  // sats_glyph-specific pixel height. The fractional path emits a '.'
  // char among the digits and never sets is_sats — '.' renders via the
  // standard kDigit path with kDigitRef, which already centres the dot
  // visually next to the digits (see price_layout.hpp's parity comment).
  // Blank pad cells stay blank after ClearFb.
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    if (now.is_sats[i]) {
      slots[panel_idx] = PaintSlot{PaintSlot::kSatsGlyph,
                                   std::string(glyph.c_str()), nullptr, 0, 0};
    } else {
      slots[panel_idx] = PaintSlot{
          PaintSlot::kDigit, std::string(1, now.digits[i]), nullptr, 0, 0};
    }
    update[panel_idx] = cell_diff_reset || full_refresh_mode ||
                        now.digits[i] != before.digits[i] ||
                        now.is_sats[i] != before.is_sats[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh_mode,
                  vertical_desc);
}

template void RenderMoscowTimeScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&, const std::string&,
    uint8_t, bool, bool, bool, bool);
template void RenderMoscowTimeScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&, const std::string&,
    uint8_t, bool, bool, bool, bool);

}  // namespace btclock
