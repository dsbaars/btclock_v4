#include "screens/screens.hpp"

#include <array>
#include <cstring>

#include "screens/common.hpp"
#include "screens/fee_rate_layout.hpp"

namespace btclock {

// Block fee-rate screen — sats/vB median mempool fee from the data hub.
// Prefers the precise `block_fee_precise` (double, from the `blockfee2`
// WS v2 subscription or the Nostr d=medianFee event) over the rounded
// `block_fee` integer; the screen manager does the field selection and
// hands this renderer a double. See fee_rate_layout.hpp for the format
// rules and the historical note on why the dot-inclusive ref stays local
// to this screen.
//
// Layout (matches old firmware `parseBlockFees`):
//   Panel 0            — "FEE/RATE" split-text label
//   Panels 1..N-2      — fee digits, right-justified ("X.YY" or int)
//   Panel N-1          — "sat" / "vB" unit split-text (Bug 4 — the
//                        literal "sat/vB" single-line label was too
//                        cramped and the `/` divider didn't line up
//                        with the FEE/RATE paired label; split-text
//                        makes them a visual pair).
//
// Partial refresh: label and unit are static after first paint; only
// the digit panels with a glyph change get flagged for repaint.

namespace {
// Ref-char string for the split-text unit glyphs. Includes exactly the
// codepoints in the text (sorted + deduped). A focused ref avoids
// picking up descenders from unrelated glyphs, so the unit's baseline
// lands consistently inside the split-text panel.
constexpr const char* kUnitRef = "Bastv";
// Pixel height for the unit split-text (top="sat", bottom="vB"). Matches
// the split-text body size used by the FEE/RATE label panel so the
// label and unit panels look like a paired frame around the digits.
constexpr float kUnitPx = 54.0f;
}  // namespace

template <size_t N>
void RenderFeeRateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    double fee_sats_vb, double prev_fee_sats_vb) {
  static_assert(N >= 7, "fee-rate layout needs at least 7 panels");
  constexpr size_t kDigitPanels = kFeeRateDigitPanels<N>;
  constexpr size_t kUnitPanel = N - 1;

  const bool full_refresh = (prev_fee_sats_vb < 0.0);

  // Panel 0 — "FEE/RATE" label. Drawn once on full refresh.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    // Inherit the digit font so the WASM preview's swappable antonio
    // slot carries the label too (Bug 1 — see block_height.cpp).
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "FEE",
                  "RATE",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.antonio(), 54.0f, /*white_text=*/false);
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
      // Use the dot-inclusive ref scoped to this screen so the "X.YY"
      // rendering keeps a consistent baseline across digit panels —
      // one of which may be '.'. Do NOT widen kDigitRef in common.hpp;
      // the comma/colon descenders would drop every other screen's
      // baseline (see test_host/test_screen_ref_chars).
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kFeeRateDotRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
    }
  }

  // Panel N-1 — "sat" over "vB" split-text unit. Static after first paint.
  // Inherits the digit font (Bug 1) so the WASM preview's font swap
  // carries through to the unit label too.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, kUnitPanel);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "sat", "vB",
                  kUnitRef, fonts.antonio(), kUnitPx,
                  /*white_text=*/false);
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->DrawFramebufferStart(fb_storage[1 + i], kind);
  }
  if (full_refresh) {
    panels[kUnitPanel]->DrawFramebufferStart(fb_storage[kUnitPanel], kind);
  }
  if (full_refresh) panels[0]->WaitForRefresh();
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->WaitForRefresh();
  }
  if (full_refresh) panels[kUnitPanel]->WaitForRefresh();
}

template void RenderFeeRateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, double, double);
template void RenderFeeRateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, double, double);

}  // namespace btclock
