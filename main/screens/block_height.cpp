#include <array>
#include <cstring>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {

template <size_t N>
void RenderBlockHeightScreen(
    std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height, bool full_refresh_mode,
    bool vertical_desc) {
  static_assert(N >= 7, "block-height layout needs at least 7 panels");

  // `cell_diff_reset` forces every cell to repaint this frame (no per-
  // cell "unchanged → skip" shortcut). Triggered by a sentinel prev_value
  // (0 here, since 0 is never a real mainnet height) or when the layout
  // flipped into/out of label-drop. Separate from `full_refresh_mode`
  // (EPD refresh kind) so a screen transition can repaint every cell
  // while still doing a partial EPD refresh.
  const bool cell_diff_reset = (prev_height == 0);
  const bool now_overflow = BlockHeightDropsLabel(block_height, N);
  const bool prev_overflow =
      !cell_diff_reset && BlockHeightDropsLabel(prev_height, N);
  // Transitioning into or out of label-drop forces a cell repaint: the
  // label panel's content changes (digit <-> "BLOCK/HEIGHT") and every
  // digit panel shifts position. EPD refresh mode is still caller-driven.
  const bool layout_changed = now_overflow != prev_overflow;
  const bool cell_force = cell_diff_reset || layout_changed;

  const size_t digit_panels = now_overflow ? N : N - 1;
  const size_t prev_digit_panels = prev_overflow ? N : N - 1;
  const size_t digit_base = now_overflow ? 0 : 1;

  char new_digits[N];
  char old_digits[N];
  FormatDigits(block_height, new_digits, digit_panels);
  if (!cell_diff_reset && !layout_changed) {
    FormatDigits(prev_height, old_digits, prev_digit_panels);
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — label in the non-overflow case. In the 7-digit overflow
  // case panel 0 becomes the leading digit; that's the `digit_base=0`
  // branch below.
  if (!now_overflow) {
    slots[0] = PaintSlot{PaintSlot::kLabelSplit, "BLOCK/HEIGHT", nullptr, 0, 0};
    update[0] = cell_force || full_refresh_mode;
  }

  // Digit panels — right-justified across `digit_panels` cells starting
  // at `digit_base`. FormatDigits has already emitted ' ' for leading
  // pad positions; PaintSlot::kDigit treats ' ' as "don't paint" so
  // leading cells stay blank after the ClearFb.
  for (size_t i = 0; i < digit_panels; ++i) {
    const size_t panel_idx = digit_base + i;
    slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                 std::string(1, new_digits[i]), nullptr, 0, 0};
    update[panel_idx] =
        cell_force || full_refresh_mode || new_digits[i] != old_digits[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh_mode,
                  vertical_desc);
}

template void RenderBlockHeightScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool);
template void RenderBlockHeightScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool);

}  // namespace btclock
