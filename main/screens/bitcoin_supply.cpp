#include "screens/screens.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "screens/common.hpp"

namespace btclock {

// parseBitcoinSupply port. Three modes, all char-per-panel so the
// existing full-size digit renderer can be reused — no smaller-font
// pass needed until Phase 2:
//
//   show_percent=true (overrides big_chars)
//     → "NN.NN" supply percentage spread over the inner panels +
//       a trailing " % " label on the last panel. Matches old
//       firmware's showPercentage branch and the parity helper
//       RenderBitcoinSupplyPercentage.
//
//   big_chars=true  → FormatNumberWithSuffix "19.9M" one char per
//                     panel, right-justified. Matches the bigChars
//                     branch (RenderBitcoinSupplyBigChars parity).
//
//   big_chars=false → plain integer BTC digits (legacy path). Still
//                     silently truncates for real mainnet (19.6M = 8
//                     digits); old firmware used small-char 3-digit
//                     groups — tracked as btclock_v3_fci-33e.

template <size_t N>
void RenderBitcoinSupplyScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height,
    bool big_chars, bool show_percent) {
  static_assert(N >= 7, "supply layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = (prev_height == 0);

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BTC",
                  "SUPPLY",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  const uint64_t now_supply = SupplyAtBlock(block_height);
  const uint64_t prev_supply =
      full_refresh ? 0 : SupplyAtBlock(prev_height);

  // Per-panel string (one entry per digit panel). Slot 0 = label,
  // handled above. Each entry is either a single char ' ' / digit /
  // symbol, or the special " % " label for the percentage trailer.
  struct Cell { char c; bool is_percent_label; };
  Cell new_cells[kDigitPanels];
  Cell old_cells[kDigitPanels];
  for (size_t i = 0; i < kDigitPanels; ++i) {
    new_cells[i] = {' ', false};
    old_cells[i] = {' ', false};
  }

  auto build_cells = [](uint64_t supply, bool big, bool pct,
                        Cell* cells) {
    if (pct) {
      // Percent form: compute NN.NN, pad to (kDigitPanels+1) chars so
      // the first padded char sits in the (ignored) label slot, then
      // copy chars [1..kDigitPanels] into cells. Last slot is overwritten
      // with " % ".
      const double frac =
          std::round((static_cast<double>(supply) / 20999999.9769) *
                     10000.0) / 100.0;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.2f%%", frac);
      std::string s = buf;
      const size_t full_slots = kDigitPanels + 1;
      if (s.size() < full_slots) {
        s.insert(s.begin(), full_slots - s.size(), ' ');
      } else if (s.size() > full_slots) {
        s = s.substr(s.size() - full_slots);
      }
      for (size_t i = 0; i < kDigitPanels; ++i) {
        cells[i] = {s[i + 1], false};
      }
      cells[kDigitPanels - 1] = {' ', true};
      return;
    }
    if (big) {
      const int num_chars = static_cast<int>((kDigitPanels + 1) - 2);
      std::string s = FormatNumberWithSuffix(supply, num_chars);
      const size_t full_slots = kDigitPanels + 1;
      if (s.size() < full_slots) {
        s.insert(s.begin(), full_slots - s.size(), ' ');
      } else if (s.size() > full_slots) {
        s = s.substr(s.size() - full_slots);
      }
      for (size_t i = 0; i < kDigitPanels; ++i) {
        cells[i] = {s[i + 1], false};
      }
      return;
    }
    // Legacy integer digits.
    char digits[16];
    FormatDigits64(supply, digits,
                   kDigitPanels < sizeof(digits) ? kDigitPanels : sizeof(digits));
    for (size_t i = 0; i < kDigitPanels; ++i) cells[i] = {digits[i], false};
  };

  build_cells(now_supply, big_chars, show_percent, new_cells);
  if (!full_refresh) {
    build_cells(prev_supply, big_chars, show_percent, old_cells);
  }

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh ||
                new_cells[i].c != old_cells[i].c ||
                new_cells[i].is_percent_label !=
                    old_cells[i].is_percent_label;
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    const auto& cell = new_cells[i];
    if (cell.is_percent_label) {
      // " % " label on the trailing panel — paint at split-text scale so
      // it reads as a unit label, not a digit (matches the fee-rate
      // "sat/vB" trailing-label pattern).
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, "%",
                       "%", fonts.antonio(), 180.0f, /*white_text=*/false);
    } else if (cell.c != ' ') {
      const char one[2] = {cell.c, '\0'};
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

template void RenderBitcoinSupplyScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool);
template void RenderBitcoinSupplyScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool);

}  // namespace btclock
