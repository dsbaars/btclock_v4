#include "screens/screens.hpp"

#include <array>
#include <string>

#include "screens/common.hpp"

namespace btclock {

// "Moscow time" = round(1e8 / price_usd) = sats per USD, shown as the
// raw integer right-justified across the digit panels with an optional
// sats glyph placed one panel before the first digit.
//
// Digit-slot count is N-1 (all panels after the label). Bug 3 — the
// previous layout hard-coded 6 digit slots so the 8-panel V8 variant
// left panel 7 blank; the old firmware parseSatsPerCurrency instead
// uses every slot (pad + digits, STS marker just before the first
// digit), which is what V8 hardware needs to look like.

template <size_t N>
void RenderMoscowTimeScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price, uint8_t sats_variant,
    bool use_sats_symbol, bool use_mscw_time) {
  static_assert(N >= 7, "Moscow-time layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;
  const bool full_refresh = prev_price.empty();

  const int32_t new_sats = SatsPerUnit(price);
  const int32_t old_sats = full_refresh ? -1 : SatsPerUnit(prev_price);

  // use_sats_symbol=false feeds `use_symbol=false` into the layout so
  // the marker cell never gets flagged — the EPD paints a blank there.
  const auto now =
      ComputeMoscowLayoutN<kDigitPanels>(new_sats, use_sats_symbol);
  const auto before =
      ComputeMoscowLayoutN<kDigitPanels>(old_sats, use_sats_symbol);

  const auto glyph = SatsGlyphUtf8(sats_variant);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — label. USD in the classic Moscow-time range (> 1 USD per
  // sat) gets "MSCW/TIME"; otherwise "SATS/<CCY>". useMscwTime=false
  // forces SATS/<CCY> regardless of range — matches the old firmware's
  // opt-out (some users prefer the uniform label). Label doesn't change
  // across price updates within the same range, so only paint on full
  // refresh — mirrors the pre-refactor "if (full_refresh)" guard.
  const bool moscow =
      use_mscw_time && currency == "USD" && new_sats > 0 && new_sats < 100000;
  const std::string label_text =
      moscow ? std::string("MSCW/TIME") : (std::string("SATS/") + currency);
  slots[0] = PaintSlot{PaintSlot::kLabelSplit, label_text, nullptr, 0, 0};
  update[0] = full_refresh;

  // Digit panels — right-justified across `kDigitPanels` cells starting
  // at panel 1. `is_sats[i]` flags the sats-glyph cell one slot before
  // the first digit; paint it via kSatsGlyph at the sats_glyph-specific
  // pixel height. Blank pad cells stay blank after ClearFb.
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    if (now.is_sats[i]) {
      slots[panel_idx] = PaintSlot{PaintSlot::kSatsGlyph,
                                   std::string(glyph.c_str()),
                                   nullptr, 0, 0};
    } else {
      slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                   std::string(1, now.digits[i]),
                                   nullptr, 0, 0};
    }
    update[panel_idx] = full_refresh ||
                        now.digits[i] != before.digits[i] ||
                        now.is_sats[i] != before.is_sats[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderMoscowTimeScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, uint8_t, bool, bool);
template void RenderMoscowTimeScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, uint8_t, bool, bool);

}  // namespace btclock
