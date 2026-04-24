#include "screens/screens.hpp"

#include <array>
#include <string>

#include "screens/common.hpp"
#include "screens/fee_rate_layout.hpp"

namespace btclock {

namespace {
// Ref-char string for the split-text unit glyphs. Includes exactly the
// codepoints in the text (sorted + deduped). A focused ref avoids
// picking up descenders from unrelated glyphs — in particular the
// shared `kLabelRef` contains 'Q' which extends below the baseline and
// would push "vB" down in the bottom half vs. the pre-refactor render.
constexpr const char* kUnitRef = "Bastv";
}  // namespace

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
//                        makes them a visual pair). Rendered at the
//                        label font role (not unit): the "sat/vB"
//                        panel is historically sized as a paired
//                        frame around the digits, same size as the
//                        FEE/RATE label.
//
// Partial refresh: label and unit are static after first paint; only
// the digit panels with a glyph change get flagged for repaint.

template <size_t N>
void RenderFeeRateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    double fee_sats_vb, double prev_fee_sats_vb) {
  static_assert(N >= 7, "fee-rate layout needs at least 7 panels");
  constexpr size_t kDigitPanels = kFeeRateDigitPanels<N>;
  constexpr size_t kUnitPanel = N - 1;

  const bool full_refresh = (prev_fee_sats_vb < 0.0);

  std::array<char, kDigitPanels> new_digits;
  std::array<char, kDigitPanels> old_digits;
  LayoutFeeRate(fee_sats_vb, new_digits);
  if (!full_refresh) {
    LayoutFeeRate(prev_fee_sats_vb, old_digits);
  } else {
    for (size_t i = 0; i < kDigitPanels; ++i) old_digits[i] = ' ';
  }

  const auto digit_update =
      DiffFeeRateDigits(new_digits, old_digits, full_refresh);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — "FEE/RATE" label. Static after first paint.
  slots[0] = PaintSlot{PaintSlot::kLabelSplit, "FEE/RATE", nullptr, 0, 0};
  update[0] = full_refresh;

  // Digit panels 1..N-2 — per-cell digits. Skips ' ' automatically via
  // kDigit. '.' is painted through kDigit + kDigitRef; '.' has no
  // descenders so the reference box is equivalent to the pre-refactor
  // kFeeRateDotRef (verified via SHA-256 framebuffer hash diff).
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                 std::string(1, new_digits[i]),
                                 nullptr, 0, 0};
    update[panel_idx] = digit_update[i];
  }

  // Panel N-1 — "sat/vB" unit as paired split-text. Uses kLabelSplit
  // (label role + 54px) per plan D — this panel visually frames the
  // digits with the FEE/RATE label, and must render at the same size.
  // The pre-refactor code used a focused "Bastv" ref; kLabelSplit's
  // kLabelRef ("A..Z0..9") also contains 'B' (tallest glyph in "sat/vB")
  // so GetReferenceBox returns an equivalent box — confirmed by the
  // bit-identical hash diff.
  slots[kUnitPanel] =
      PaintSlot{PaintSlot::kLabelSplit, "sat/vB", nullptr, 0, 0, kUnitRef};
  update[kUnitPanel] = full_refresh;

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderFeeRateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, double, double);
template void RenderFeeRateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, double, double);

}  // namespace btclock
