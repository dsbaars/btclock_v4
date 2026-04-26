// Firmware OTA overlay — painted once at the start of a push-upload.
//
// Every panel carries the same "UP/DATE" split-text label (top half
// "UP", bottom half "DATE"). Full refresh only; the screen is never
// diffed — it's stomped once on entry and stays until esp_restart
// fires. Host-side dependency surface is identical to show_custom.cpp
// so the Rev A / Rev B / V8 instantiations sweep across the shared
// template.

#include <array>
#include <cstddef>
#include <string>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {
namespace {

// Reference character set for split-text centering. Matches the one
// show_custom / screen-label code uses elsewhere so the baseline sits
// at the same pixel height as the other split-text screens when the
// device reboots into the newly-flashed firmware.
constexpr const char* kAnyRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

template <size_t N>
void PaintUpdatePanel(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                      uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
                      size_t i) {
  auto lfb = PrepFb(panels, fb_storage, i);
  ClearFb(lfb, /*white=*/true);
  const char* top = "UP";
  const char* bot = "DATE";
  const int target = lfb.native_width - 6;
  // Fit both halves to the same pixel height so the letters line up —
  // pick the smaller of the two computed sizes; matches the sizing
  // choice in show_custom's split-text branch.
  const float px_top =
      FitTextPx(top, fonts.oswald_bold(), 54.0f, 14.0f, target);
  const float px_bot =
      FitTextPx(bot, fonts.oswald_bold(), 54.0f, 14.0f, target);
  const float px = px_top < px_bot ? px_top : px_bot;
  DrawSplitText(lfb, lfb.native_width, lfb.native_height, top, bot, kAnyRef,
                fonts.oswald_bold(), px, /*white_text=*/false);
}

}  // namespace

template <size_t N>
void RenderOtaUpdateScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                           uint8_t (&fb_storage)[N][16 * 296],
                           const AppFonts& fonts) {
  for (size_t i = 0; i < N; ++i) {
    PaintUpdatePanel(panels, fb_storage, fonts, i);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->DrawFramebufferStart(fb_storage[i], RefreshKind::kFull);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

template void RenderOtaUpdateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&);
template void RenderOtaUpdateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&);

}  // namespace btclock
