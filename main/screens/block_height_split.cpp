#include <array>
#include <cstring>
#include <string>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {

namespace {

// Build the "TOP/BOTTOM" payload a PaintSlot::kDigitStack cell expects.
// ' ' is FormatDigits' leading-pad marker; map it to an empty half so
// DrawStackedText skips the row entirely instead of measuring a space.
std::string StackCell(char top, char bottom) {
  std::string s;
  if (top != ' ') s.push_back(top);
  s.push_back('/');
  if (bottom != ' ') s.push_back(bottom);
  return s;
}

}  // namespace

template <size_t N>
void RenderBlockHeightSplitScreen(
    std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t bip110_height, uint32_t prev_height,
    uint32_t prev_bip110_height, bool full_refresh_mode, bool vertical_desc) {
  static_assert(N >= 7, "dual block-height layout needs at least 7 panels");

  // Sentinel prev (0) forces every cell to repaint — same contract as
  // RenderBlockHeightScreen. Both rows share one reset flag because the
  // label cell is shared and a half-repainted strip would mix frames.
  const bool cell_diff_reset = (prev_height == 0);
  const bool cell_force = cell_diff_reset;

  // Unlike the single-height screen there is no label-drop path: the
  // label cell also carries the row legend ("BTC" over "BIP110"), and
  // without it the two digit rows are unidentifiable. A 7-digit height
  // therefore truncates its leading digit here (FormatDigits' documented
  // behaviour) rather than reclaiming panel 0 — the next decade rollover
  // is years out, and this screen is about the delta between the rows,
  // which the low-order digits carry.
  constexpr size_t kDigitSlots = N - 1;

  char top_new[N];
  char bot_new[N];
  char top_old[N];
  char bot_old[N];
  FormatDigits(block_height, top_new, kDigitSlots);
  // 0 = "no sample yet" → blank row, not a rendered "0".
  if (bip110_height == 0) {
    std::memset(bot_new, ' ', kDigitSlots);
  } else {
    FormatDigits(bip110_height, bot_new, kDigitSlots);
  }
  if (!cell_diff_reset) {
    FormatDigits(prev_height, top_old, kDigitSlots);
    if (prev_bip110_height == 0) {
      std::memset(bot_old, ' ', kDigitSlots);
    } else {
      FormatDigits(prev_bip110_height, bot_old, kDigitSlots);
    }
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // kLabelStack, not kLabelSplit: the legend has to sit on the same two
  // rows as the digit cells and carry the same edge-to-edge rule, which
  // also means it stays unrotated regardless of `vertical_desc`.
  slots[0] = PaintSlot{PaintSlot::kLabelStack, "BTC/BIP110", nullptr, 0, 0};
  update[0] = cell_force || full_refresh_mode;

  for (size_t i = 0; i < kDigitSlots; ++i) {
    const size_t panel_idx = 1 + i;
    slots[panel_idx] =
        PaintSlot{PaintSlot::kDigitStack, StackCell(top_new[i], bot_new[i]),
                  nullptr, 0, 0};
    // A cell repaints when either row's glyph moved — the two chains
    // advance independently, so diffing only the canonical row would
    // leave the BIP-110 row stale for a whole block.
    update[panel_idx] = cell_force || full_refresh_mode ||
                        top_new[i] != top_old[i] || bot_new[i] != bot_old[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh_mode,
                  vertical_desc);
}

template void RenderBlockHeightSplitScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool);
template void RenderBlockHeightSplitScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool);

}  // namespace btclock
