#include "screens/screens.hpp"

#include <cstring>

#include "screens/common.hpp"

namespace btclock {

template <size_t N>
void RenderBlockHeightScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height) {
  static_assert(N >= 7, "block-height layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = (prev_height == 0);

  // Panel 0 — "BLOCK/HEIGHT" label. Label never changes after first paint.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BLOCK",
                  "HEIGHT",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  char new_digits[kDigitPanels];
  char old_digits[kDigitPanels];
  FormatDigits(block_height, new_digits, kDigitPanels);
  if (!full_refresh) {
    FormatDigits(prev_height, old_digits, kDigitPanels);
  }

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh || new_digits[i] != old_digits[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kDigitRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
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

template void RenderBlockHeightScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t);
template void RenderBlockHeightScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t);

}  // namespace btclock
