#include "screens/screens.hpp"

#include "screens/common.hpp"

namespace btclock {

// Blocks-remaining-to-next-halving. Old firmware (parseHalvingCountdown
// in lib/btclock/data_handler.cpp) supports both "blocks" and
// "years/days/hours/mins" display modes; we ship the blocks mode first
// because its panel layout reuses block_height.cpp verbatim. The other
// mode needs per-panel labels ("3/YRS", "12/DAYS", …) which is a
// different layout problem — tracked as a follow-up on lx0.14.

template <size_t N>
void RenderHalvingScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height) {
  static_assert(N >= 7, "halving layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = (prev_height == 0);

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "HAL",
                  "VING",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  const uint32_t now_rem = HalvingCountdown(block_height);
  const uint32_t prev_rem =
      full_refresh ? 0 : HalvingCountdown(prev_height);

  char new_digits[kDigitPanels];
  char old_digits[kDigitPanels];
  FormatDigits(now_rem, new_digits, kDigitPanels);
  if (!full_refresh) FormatDigits(prev_rem, old_digits, kDigitPanels);

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

template void RenderHalvingScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t);
template void RenderHalvingScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t);

}  // namespace btclock
