#include "screens/screens.hpp"

#include <cstdio>
#include <string>

#include "screens/common.hpp"

namespace btclock {

// parseHalvingCountdown port. Two modes, matching the old firmware's
// asBlocks flag:
//
//   as_blocks=true  — blocks-remaining; panel 0 "HAL/VING" label,
//                     remaining panels carry right-justified digits.
//   as_blocks=false — time breakdown; panels 0..1 carry "BIT/COIN"
//                     + "HAL/VING" split-text headers, panels 2..5
//                     carry "N/YRS", "N/DAYS", "N/HRS", "N/MINS",
//                     panel 6 "TO/GO".
//
// Time-mode uses DrawSplitText for per-panel "N/UNIT" labels so both
// the number and the unit suffix share the panel — same split-text
// style the FEE/RATE and BIT/COIN headers use. Diff-based partial
// refresh keeps only changed panels repainting.

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
// and the unit label (UNIT) floats underneath. Uses the digit font
// (antonio) so the WASM preview's font-family swap carries through —
// see block_height.cpp for the full Bug 1 note.
void DrawSlashSplit(auto& lfb, const AppFonts& fonts,
                    const std::string& cell) {
  const std::size_t slash = cell.find('/');
  if (slash == std::string::npos) {
    // Defensive: emit the raw string if slashless — matches the block
    // of parsed text the label builders produce.
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                     cell.c_str(),
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                     fonts.antonio(), 54.0f, /*white_text=*/false);
    return;
  }
  const std::string top = cell.substr(0, slash);
  const std::string bot = cell.substr(slash + 1);
  // Inherit the digit font so split-text slots follow the user's
  // font pick in the preview (Bug 1 — see block_height.cpp).
  DrawSplitText(lfb, lfb.native_width, lfb.native_height,
                top.c_str(), bot.c_str(),
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                fonts.antonio(), 54.0f, /*white_text=*/false);
}

}  // namespace

template <size_t N>
void RenderHalvingScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height, bool as_blocks) {
  static_assert(N >= 7, "halving layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = (prev_height == 0);

  if (as_blocks) {
    if (full_refresh) {
      auto lfb = PrepFb(panels, fb_storage, 0);
      ClearFb(lfb, /*white=*/true);
      // Inherit the digit font so the label follows the preview font
      // pick (Bug 1 — see block_height.cpp).
      DrawSplitText(lfb, lfb.native_width, lfb.native_height, "HAL",
                    "VING",
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                    fonts.antonio(), 54.0f, /*white_text=*/false);
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
    return;
  }

  // --- time-breakdown mode ---
  // On an 8-panel board the 7 layout slots shift right by one so the
  // "BIT/COIN|HAL/VING" header stays glued and slot 2 (the first digit
  // position on the 7-panel form) becomes blank on the 8-panel form.
  // This mirrors what panel_texts.cpp emits for the WebUI status.
  const auto new_slots = TimeModeSlots(block_height);
  const auto old_slots = full_refresh
                             ? std::array<std::string, 7>{}
                             : TimeModeSlots(prev_height);
  constexpr size_t kSlotOffset = N - 7;

  std::array<bool, N> update{};
  for (size_t i = 0; i < N; ++i) {
    if (i < kSlotOffset) {
      // Leading blank panels on the 8-panel variant — paint them blank
      // on full-refresh only.
      update[i] = full_refresh;
      continue;
    }
    const size_t slot = i - kSlotOffset;
    update[i] = full_refresh || new_slots[slot] != old_slots[slot];
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
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
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
    const AppFonts&, uint32_t, uint32_t, bool);
template void RenderHalvingScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool);

}  // namespace btclock
