#include "screens/screens.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "screens/common.hpp"

namespace btclock {

// parseBitcoinSupply port. Three modes:
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
//   big_chars=false → 3-digit-group small-chars layout (SmallCharsGroups
//                     in screen_math). Each trailing panel renders up
//                     to three digits at medium font; matches the old
//                     firmware small-chars path and the panel-texts
//                     mirror for /api/status data[].

template <size_t N>
void RenderBitcoinSupplyScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    uint32_t block_height, uint32_t prev_height,
    bool big_chars, bool show_percent,
    bool full_refresh_mode, bool vertical_desc) {
  static_assert(N >= 7, "supply layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  // `cell_diff_reset` forces every cell to repaint (sentinel prev_height
  // 0); `full_refresh_mode` drives the EPD refresh kind. See screens.hpp.
  const bool cell_diff_reset = (prev_height == 0);

  const uint64_t now_supply = SupplyAtBlock(block_height);
  const uint64_t prev_supply =
      cell_diff_reset ? 0 : SupplyAtBlock(prev_height);

  // Per-panel string (one entry per digit panel). Slot 0 = label.
  // Each entry is either:
  //   - a single char (digit or ' ') from the percentage / big-chars
  //     layouts
  //   - a 3-char group "NNN" / "  1" from the small-chars layout
  //   - the special " % " marker that paints as a "%" digit on the
  //     trailing panel of the percent mode.
  struct Cell {
    std::string s;      // "" → don't paint; single char or 3-char group
    bool is_percent_label = false;
  };
  Cell new_cells[kDigitPanels];
  Cell old_cells[kDigitPanels];

  auto build_cells = [](uint64_t supply, bool big, bool pct,
                        Cell* cells) {
    if (pct) {
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
        cells[i].s = std::string(1, s[i + 1]);
        cells[i].is_percent_label = false;
      }
      cells[kDigitPanels - 1] = {"", true};
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
        cells[i].s = std::string(1, s[i + 1]);
        cells[i].is_percent_label = false;
      }
      return;
    }
    // Small-chars 3-digit-group layout — mirror matches via
    // EmitSmallCharsGroups → SmallCharsGroups in panel_texts.cpp.
    auto groups = SmallCharsGroups(supply, /*ccy_cell=*/"", kDigitPanels);
    for (size_t i = 0; i < kDigitPanels; ++i) {
      cells[i].s = std::move(groups[i]);
      cells[i].is_percent_label = false;
    }
  };

  build_cells(now_supply, big_chars, show_percent, new_cells);
  if (!cell_diff_reset) {
    build_cells(prev_supply, big_chars, show_percent, old_cells);
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — "BTC/SUPPLY" label. Static after first paint.
  slots[0] =
      PaintSlot{PaintSlot::kLabelSplit, "BTC/SUPPLY", nullptr, 0, 0};
  update[0] = cell_diff_reset || full_refresh_mode;

  // Digit / group cells. Single-char → kDigit (180 px digit font).
  // 3-char group → kSmallGroup (90 px small_chars font). The percent
  // marker on the trailing panel paints as a "%" digit cell — matches
  // the pre-refactor inline DrawTextCentered with digit font at 180 pt.
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    const auto& cell = new_cells[i];
    if (cell.is_percent_label) {
      slots[panel_idx] =
          PaintSlot{PaintSlot::kDigit, std::string("%"), nullptr, 0, 0};
    } else if (cell.s.empty()) {
      slots[panel_idx] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
    } else if (cell.s.size() == 1) {
      // Single char → full-size digit. A ' ' here short-circuits to
      // no-paint in PaintSlotIntoFb (kDigit).
      slots[panel_idx] =
          PaintSlot{PaintSlot::kDigit, cell.s, nullptr, 0, 0};
    } else {
      // 3-char group → medium font so "NNN" fits panel-width.
      slots[panel_idx] =
          PaintSlot{PaintSlot::kSmallGroup, cell.s, nullptr, 0, 0};
    }
    update[panel_idx] = cell_diff_reset || full_refresh_mode ||
                        new_cells[i].s != old_cells[i].s ||
                        new_cells[i].is_percent_label !=
                            old_cells[i].is_percent_label;
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update,
                  full_refresh_mode, vertical_desc);
}

template void RenderBitcoinSupplyScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool, bool, bool);
template void RenderBitcoinSupplyScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, uint32_t, uint32_t, bool, bool, bool, bool);

}  // namespace btclock
