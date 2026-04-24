#include "boot_ui.hpp"

namespace btclock {
namespace {

// Letters shown per panel, left to right. 7-panel boards draw the
// 'BTCLOCK' prefix; 8-panel boards tack on '!' for the extra slot.
constexpr const char* kSplashLetters = "BTCLOCK!";

}  // namespace

template <size_t N>
void RenderSplashScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts) {
  static_assert(N <= 8, "kSplashLetters only spells out eight glyphs");

  const Font& font = fonts.digit();

  for (size_t i = 0; i < N; ++i) {
    LandscapeFb lfb = {};
    lfb.native_fb = fb_storage[i];
    lfb.native_stride = panels[i]->kStride;
    lfb.native_width = panels[i]->Width();
    lfb.native_height = panels[i]->Height();
    lfb.rotation = Rotation::k180;

    ClearFb(lfb, /*white=*/true);

    const char letter = kSplashLetters[i];
    const char one[2] = {letter, '\0'};
    // Fit the glyph as large as it goes without clipping — Oswald's
    // condensed bold lets a single letter eat most of the 122 px panel
    // width at roughly 180–200 px height.
    const float px = FitTextPx(one, font, 220.0f, 100.0f,
                                lfb.native_width - 12);
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                     kSplashLetters, font, px, /*white_text=*/false);

    panels[i]->DrawFramebufferStart(fb_storage[i], RefreshKind::kFull);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

template void RenderSplashScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&,
    uint8_t (&)[7][16 * 296], const AppFonts&);
template void RenderSplashScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&,
    uint8_t (&)[8][16 * 296], const AppFonts&);

}  // namespace btclock
