#include "screens/screens.hpp"

#include <array>
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
  // the label panel's content changes (digit <-> "BLOCK/HEIGHT") and
  // every digit panel shifts position.
  const bool layout_changed = now_overflow != prev_overflow;
  const bool label_full_refresh = full_refresh || layout_changed;

  const size_t digit_panels = now_overflow ? N : N - 1;
  const size_t prev_digit_panels = prev_overflow ? N : N - 1;
  const size_t digit_base = now_overflow ? 0 : 1;

  char new_digits[N];
  char old_digits[N];
  FormatDigits(block_height, new_digits, digit_panels);
  if (!full_refresh && !layout_changed) {
    FormatDigits(prev_height, old_digits, prev_digit_panels);
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — label in the non-overflow case. In the 7-digit overflow
  // case panel 0 becomes the leading digit; that's the `digit_base=0`
  // branch below. When overflow changes frame-to-frame the whole
  // screen goes full-refresh via `layout_changed`.
  if (!now_overflow) {
    slots[0] = PaintSlot{PaintSlot::kLabelSplit, "BLOCK/HEIGHT", nullptr, 0, 0};
    update[0] = label_full_refresh;
  }

  // Digit panels — right-justified across `digit_panels` cells starting
  // at `digit_base`. FormatDigits has already emitted ' ' for leading
  // pad positions; PaintSlot::kDigit treats ' ' as "don't paint" so
  // leading cells stay blank after the ClearFb.
  for (size_t i = 0; i < digit_panels; ++i) {
    const size_t panel_idx = digit_base + i;
    slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                 std::string(1, new_digits[i]),
                                 nullptr, 0, 0};
    update[panel_idx] = full_refresh || layout_changed ||
                        new_digits[i] != old_digits[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update,
                  label_full_refresh);
}

template void RenderBlockHeightScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t);
template void RenderBlockHeightScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t);

}  // namespace btclock
