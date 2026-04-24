#include "screens/screens.hpp"

#include <array>
#include <cstdio>
#include <string>

#include "screens/common.hpp"

namespace btclock {

// parseHalvingCountdown port. Two modes, matching the old firmware's
// asBlocks flag:
//
//   as_blocks=true  — blocks-remaining; panel 0 "HAL/VING" label,
//                     remaining panels carry right-justified digits.
//                     Uses the shared PaintDataScreen helper.
//   as_blocks=false — time breakdown; panels 0..1 carry "BIT/COIN"
//                     + "HAL/VING" split-text headers, panels 2..5
//                     carry "N/YRS", "N/DAYS", "N/HRS", "N/MINS",
//                     panel 6 "TO/GO".
//
// Time-mode uses DrawSplitText for per-panel "N/UNIT" labels so both
// the number and the unit suffix share the panel — same split-text
// style the FEE/RATE and BIT/COIN headers use. It stays out of
// PaintDataScreen per plan D.4: every panel is an ad-hoc per-screen
// split and wiring each into a PaintSlot doesn't remove meaningful
// duplication vs. just keeping the existing DrawSplitText loop.

namespace {

// Build the 7 slot strings for the time-breakdown mode (for N=7 boards
// the slot indices map 1:1 onto panels; for N=8 the slots shift right
// by one so the trailing layout is preserved and slot 2 is left blank).
std::array<std::string, 7> TimeModeSlots(uint32_t block_height) {
  const HalvingTimeBreakdown tb = HalvingCountdownBreakdown(block_height);
  std::array<std::string, 7> s;
  s[0] = "BIT/COIN";
  s[1] = "HAL/VING";
  s[7 - 5] = std::to_string(tb.years)   + "/YRS";
  s[7 - 4] = std::to_string(tb.days)    + "/DAYS";
  s[7 - 3] = std::to_string(tb.hours)   + "/HRS";
  s[7 - 2] = std::to_string(tb.minutes) + "/MINS";
  s[7 - 1] = "TO/GO";
  return s;
}

// Paint a "N/UNIT" slot as two-line split text so the number stays big
// and the unit label (UNIT) floats underneath. Uses the label role so
// the user's `fontName` pick flows through — see block_height.cpp for
// the Bug 1 note.
void DrawSlashSplit(auto& lfb, const AppFonts& fonts,
                    const std::string& cell) {
  const std::size_t slash = cell.find('/');
  if (slash == std::string::npos) {
    // Defensive: emit the raw string if slashless — matches the block
    // of parsed text the label builders produce.
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                     cell.c_str(),
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                     fonts.label(), 54.0f, /*white_text=*/false);
    return;
  }
  const std::string top = cell.substr(0, slash);
  const std::string bot = cell.substr(slash + 1);
  // Label role so split-text slots follow the user's font pick in the
  // preview (Bug 1 — see block_height.cpp).
  DrawSplitText(lfb, lfb.native_width, lfb.native_height,
                top.c_str(), bot.c_str(),
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                fonts.label(), 54.0f, /*white_text=*/false);
}

}  // namespace

template <size_t N>
void RenderHalvingScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height, bool as_blocks,
    bool full_refresh_mode, bool vertical_desc) {
  static_assert(N >= 7, "halving layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  // `cell_diff_reset` forces every cell to repaint (sentinel prev_height
  // 0 — 0 isn't a real mainnet height); `full_refresh_mode` drives the
  // EPD refresh kind. See screens.hpp.
  const bool cell_diff_reset = (prev_height == 0);

  if (as_blocks) {
    const uint32_t now_rem = HalvingCountdown(block_height);
    const uint32_t prev_rem =
        cell_diff_reset ? 0 : HalvingCountdown(prev_height);

    char new_digits[kDigitPanels];
    char old_digits[kDigitPanels];
    FormatDigits(now_rem, new_digits, kDigitPanels);
    if (!cell_diff_reset) FormatDigits(prev_rem, old_digits, kDigitPanels);

    std::array<PaintSlot, N> slots{};
    std::array<bool, N> update{};

    // Panel 0 — "HAL/VING" label. Static after first paint.
    slots[0] = PaintSlot{PaintSlot::kLabelSplit, "HAL/VING", nullptr, 0, 0};
    update[0] = cell_diff_reset || full_refresh_mode;

    // Digit panels 1..N-1 — right-justified blocks-remaining. ' ' pad
    // cells short-circuit to no paint inside kDigit.
    for (size_t i = 0; i < kDigitPanels; ++i) {
      const size_t panel_idx = 1 + i;
      slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                   std::string(1, new_digits[i]),
                                   nullptr, 0, 0};
      update[panel_idx] = cell_diff_reset || full_refresh_mode ||
                          new_digits[i] != old_digits[i];
    }

    PaintDataScreen(panels, fb_storage, fonts, slots, update,
                    full_refresh_mode, vertical_desc);
    return;
  }

  // --- time-breakdown mode ---
  // On an 8-panel board the 7 layout slots shift right by one so the
  // "BIT/COIN|HAL/VING" header stays glued and slot 2 (the first digit
  // position on the 7-panel form) becomes blank on the 8-panel form.
  // This mirrors what panel_texts.cpp emits for the WebUI status.
  //
  // Stays outside PaintDataScreen per plan D.4: every panel is a per-
  // slot DrawSplitText with slot-specific content that doesn't fit the
  // label/digit/unit shape the helper was designed around.
  const auto new_slots = TimeModeSlots(block_height);
  const auto old_slots = cell_diff_reset
                             ? std::array<std::string, 7>{}
                             : TimeModeSlots(prev_height);
  constexpr size_t kSlotOffset = N - 7;

  std::array<bool, N> update{};
  for (size_t i = 0; i < N; ++i) {
    if (i < kSlotOffset) {
      // Leading blank panels on the 8-panel variant — paint them blank
      // on a cell-diff reset or a full EPD refresh.
      update[i] = cell_diff_reset || full_refresh_mode;
      continue;
    }
    const size_t slot = i - kSlotOffset;
    update[i] = cell_diff_reset || full_refresh_mode ||
                new_slots[slot] != old_slots[slot];
  }

  for (size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, i);
    ClearFb(lfb, /*white=*/true);
    if (i < kSlotOffset) continue;  // blank panel
    const size_t slot = i - kSlotOffset;
    DrawSlashSplit(lfb, fonts, new_slots[slot]);
  }

  const RefreshKind kind =
      full_refresh_mode ? RefreshKind::kFull : RefreshKind::kPartial;
  for (size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    panels[i]->DrawFramebufferStart(fb_storage[i], kind);
  }
  for (size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    panels[i]->WaitForRefresh();
  }
}

template void RenderHalvingScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool, bool);
template void RenderHalvingScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool, bool);

}  // namespace btclock
