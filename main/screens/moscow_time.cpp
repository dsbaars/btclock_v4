#include "screens/screens.hpp"

#include "screens/common.hpp"

namespace btclock {

// "Moscow time" = round(1e8 / price_usd) = sats per USD, shown as the
// raw integer right-justified across the digit panels with an optional
// sats glyph placed one panel before the first digit. 6 digits fit
// without a symbol.

template <size_t N>
void RenderMoscowTimeScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price, uint8_t sats_variant) {
  static_assert(N >= 7, "Moscow-time layout needs at least 7 panels");
  constexpr size_t kDigitPanels = 6;
  const bool full_refresh = prev_price.empty();

  const int32_t new_sats = SatsPerUnit(price);
  const int32_t old_sats = full_refresh ? -1 : SatsPerUnit(prev_price);

  // Panel 0 — label. USD in the classic Moscow-time range (> 1 USD per
  // sat) gets "MSCW/TIME"; otherwise "SATS/<CCY>". Label doesn't change
  // across price updates within the same range, so only on full refresh.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    const bool moscow =
        currency == "USD" && new_sats > 0 && new_sats < 100000;
    const char* top = moscow ? "MSCW" : "SATS";
    const char* bot = moscow ? "TIME" : currency.c_str();
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, top, bot,
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  const DigitLayout now = ComputeMoscowLayout(new_sats, /*use_symbol=*/true);
  const DigitLayout before =
      ComputeMoscowLayout(old_sats, /*use_symbol=*/true);

  const auto glyph = SatsGlyphUtf8(sats_variant);

  // Sats-glyph pixel-height is deliberately lower than the digit height.
  // The Satoshi Symbol font fills its em-box (ink width ≈ em-width), so
  // rendering it at the digit pixel-height would leave much less visual
  // margin around the glyph than Antonio digits get around themselves.
  // 130 keeps the glyph the same vertical size as digit ink (both ≈
  // 120-130 px) while shrinking the horizontal ink so symmetric panel
  // centering yields margins close to digit-panel margins — no manual
  // x-shift needed.
  constexpr float kSatsPixelHeight = 130.0f;

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh ||
                now.digits[i] != before.digits[i] ||
                now.is_sats[i] != before.is_sats[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (now.is_sats[i]) {
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                       glyph.c_str(), glyph.c_str(), fonts.sats_symbol(),
                       kSatsPixelHeight, /*white_text=*/false);
    } else if (now.digits[i] != ' ') {
      const char one[2] = {now.digits[i], '\0'};
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

template void RenderMoscowTimeScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, uint8_t);
template void RenderMoscowTimeScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, uint8_t);

}  // namespace btclock
