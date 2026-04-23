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

  const bool full_refresh = (prev_height == 0);
  const bool now_overflow = BlockHeightDropsLabel(block_height, N);
  const bool prev_overflow =
      !full_refresh && BlockHeightDropsLabel(prev_height, N);
  // Transitioning into or out of label-drop forces a full repaint:
  // the label panel's content changes (digit ↔ "BLOCK/HEIGHT") and
  // every digit panel shifts position.
  const bool layout_changed = now_overflow != prev_overflow;
  const bool label_full_refresh = full_refresh || layout_changed;

  const size_t digit_panels = now_overflow ? N : N - 1;
  const size_t prev_digit_panels = prev_overflow ? N : N - 1;
  const size_t digit_base = now_overflow ? 0 : 1;

  // Panel 0 — label only when the height fits within N-1 panels.
  if (label_full_refresh && !now_overflow) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BLOCK",
                  "HEIGHT",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  char new_digits[N];
  char old_digits[N];
  FormatDigits(block_height, new_digits, digit_panels);
  if (!full_refresh && !layout_changed) {
    FormatDigits(prev_height, old_digits, prev_digit_panels);
  }

  std::array<bool, N> update{};
  for (size_t i = 0; i < digit_panels; ++i) {
    update[i] = full_refresh || layout_changed ||
                new_digits[i] != old_digits[i];
  }

  for (size_t i = 0; i < digit_panels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, digit_base + i);
    ClearFb(lfb, /*white=*/true);
    if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kDigitRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
    }
  }

  const RefreshKind kind =
      label_full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (label_full_refresh && !now_overflow) {
    panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  }
  for (size_t i = 0; i < digit_panels; ++i) {
    if (!update[i]) continue;
    panels[digit_base + i]->DrawFramebufferStart(
        fb_storage[digit_base + i], kind);
  }
  if (label_full_refresh && !now_overflow) panels[0]->WaitForRefresh();
  for (size_t i = 0; i < digit_panels; ++i) {
    if (!update[i]) continue;
    panels[digit_base + i]->WaitForRefresh();
  }
}

template void RenderBlockHeightScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t);
template void RenderBlockHeightScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t);

}  // namespace btclock
