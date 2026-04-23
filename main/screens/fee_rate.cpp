#include "screens/screens.hpp"

#include <array>
#include <cstring>

#include "screens/common.hpp"
#include "screens/fee_rate_layout.hpp"

namespace btclock {

// Block fee-rate screen — integer sats/vB from the data hub's
// `block_fee` field (populated by BtclockDataSource from the WS v2
// `blockfee` subscription). Layout mirrors block-height / btc-price:
// panel 0 holds the label, panels 1..N-1 hold the digits.
//
// We intentionally skip the old firmware's "sat/vB" unit on the last
// panel — the fonts component doesn't ship a dedicated glyph for it
// yet, and rendering "sat/vB" as Antonio text at digit-panel scale
// looked cramped in bring-up. Adding it is tracked in the same beads
// issue (btclock_v3_fci-wbr); see fee_rate_layout.hpp for the steps.
//
// Decimal (blockfee2) variant is tracked separately by
// btclock_v3_fci-znf.

template <size_t N>
void RenderFeeRateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    int32_t fee_sats_vb, int32_t prev_fee_sats_vb) {
  static_assert(N >= 7, "fee-rate layout needs at least 7 panels");
  constexpr size_t kDigitPanels = kFeeRateDigitPanels<N>;

  const bool full_refresh = (prev_fee_sats_vb < 0);

  // Panel 0 — "FEE/RATE" label. Same split-text style as block-height
  // and btc-price. Drawn once on full refresh and left alone thereafter.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "FEE",
                  "RATE",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  std::array<char, kDigitPanels> new_digits;
  std::array<char, kDigitPanels> old_digits;
  LayoutFeeRate(fee_sats_vb, new_digits);
  if (!full_refresh) {
    LayoutFeeRate(prev_fee_sats_vb, old_digits);
  } else {
    for (size_t i = 0; i < kDigitPanels; ++i) old_digits[i] = ' ';
  }

  const auto update =
      DiffFeeRateDigits(new_digits, old_digits, full_refresh);

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      // kDigitRef is the shared integer ref — keeps the digits on this
      // screen at the same vertical baseline as block-height / Moscow-
      // time / btc-price digits (see common.hpp for the descender
      // caveat, and test_host/test_screen_ref_chars for the regression).
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

template void RenderFeeRateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, int32_t, int32_t);
template void RenderFeeRateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, int32_t, int32_t);

}  // namespace btclock
