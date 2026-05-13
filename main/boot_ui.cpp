#include "boot_ui.hpp"

#include "fit_text_px.hpp"

namespace btclock {
namespace {

// Letters shown per panel, left to right. 7-panel boards draw the
// 'BTCLOCK' prefix; 8-panel boards tack on '!' for the extra slot.
constexpr const char* kSplashLetters = "BTCLOCK!";

}  // namespace

template <size_t N>
void RenderSplashScreen(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
                        uint8_t (&fb_storage)[N][16 * 296],
                        const AppFonts& fonts) {
  static_assert(N <= 8, "kSplashLetters only spells out eight glyphs");

  const Font& font = fonts.digit();

  // Same pixel height on every panel — match digit-screen behaviour (one
  // scale for the whole row). The limiting glyph is whichever needs the
  // smallest FitTextPx to stay inside panel width minus margin.
  //
  // The 20-px margin (10 px each side on the 122-px short axis) is sized
  // so the condensed display faces — Antonio (widest 'K' ink ≈63 px @
  // em=220), Oswald (widest 'O' ≈67 px) — still hit the kSplashMaxPx
  // ceiling exactly as before, while the wide / monospace faces (Azeret
  // ~112 px, Atkinson ~112 px, Inter ~123 px) shrink one fit-step further
  // so their ink never crowds the panel edge. Anything tighter (≤ 14 px)
  // leaves Azeret with single-digit pixel gaps; the boot splash is the
  // first impression and that looked cramped on device.
  constexpr float kSplashMaxPx = 220.0f;
  constexpr float kSplashMinPx = 100.0f;
  constexpr int kSplashMarginPx = 20;
  const float uniform_px = MinGlyphFitPxAcross(N, [&](std::size_t i) {
    const char one[2] = {kSplashLetters[i], '\0'};
    const int target_w = panels[i]->Width() - kSplashMarginPx;
    return FitTextPx(one, font, kSplashMaxPx, kSplashMinPx, target_w);
  });

  char ref_visible[N + 1];
  for (size_t i = 0; i < N; ++i) ref_visible[i] = kSplashLetters[i];
  ref_visible[N] = '\0';

  for (size_t i = 0; i < N; ++i) {
    LandscapeFb lfb = {};
    lfb.native_fb = fb_storage[i];
    lfb.native_stride = panels[i]->kStride;
    lfb.native_width = panels[i]->Width();
    lfb.native_height = panels[i]->Height();
    lfb.rotation = Rotation::k180;

    ClearFb(lfb, /*white=*/true);

    const char letter = kSplashLetters[i];
    const char cell[2] = {letter, '\0'};
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, cell,
                     ref_visible, font, uniform_px, /*white_text=*/false);

    panels[i]->DrawFramebufferStart(fb_storage[i], RefreshKind::kFull);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

template void RenderSplashScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&);
template void RenderSplashScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&);

}  // namespace btclock
