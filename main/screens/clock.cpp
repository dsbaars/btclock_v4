#include <array>
#include <cstdio>
#include <string>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {

// Panel 0 = dd/mm date (split-text, matches the old firmware's
// `std::to_string(mday)+"/"+std::to_string(mon+1)` payload that
// EPDManager then routes through splitText on the '/' separator).
// Panels 1..N-1 = HH:MM digits right-justified, with ':' in the
// slot between HH and MM. If wall-clock isn't yet plausible
// (tv_sec predates 2020 — SNTP still pending) the time panels are
// blanked rather than showing an epoch glitch.
//
// '0'..'9' use kDigitRef; ':' falls through kDigit + kDigitRef too —
// the pre-refactor code did the same (ref_chars = kDigitRef for all
// cells, incl. ':'). ':' is rendered as its own two-dot glyph whose
// vertical position is governed by the digit ref box; it floats
// between the digit rows at the digit baseline.

template <size_t N>
void RenderClockScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                       uint8_t (&fb_storage)[N][16 * 296],
                       const AppFonts& fonts, bool valid, int hour, int minute,
                       int mday, int month, bool prev_valid, int prev_hour,
                       int prev_minute, int prev_mday, int prev_month,
                       bool full_refresh_mode, bool vertical_desc,
                       bool hide_leading_zero) {
  static_assert(N >= 7, "clock layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  // `cell_diff_reset` forces every cell to repaint on first paint / date
  // rollover / validity flip; `full_refresh_mode` drives the EPD refresh
  // kind. See screens.hpp.
  const bool date_changed =
      !prev_valid || prev_mday != mday || prev_month != month;
  const bool cell_diff_reset = (!prev_valid && valid) || date_changed;

  // Build the date-label text. When time isn't yet plausible we draw an
  // em-dash split to tell the user the screen exists but NTP hasn't
  // landed yet.
  char top[4];
  char bot[4];
  if (valid) {
    std::snprintf(top, sizeof(top), "%d", mday);
    std::snprintf(bot, sizeof(bot), "%d", month);
  } else {
    std::snprintf(top, sizeof(top), "-");
    std::snprintf(bot, sizeof(bot), "-");
  }
  std::string label_text = std::string(top) + "/" + bot;

  const ClockLayout now =
      ComputeClockLayout(valid, hour, minute, kDigitPanels, hide_leading_zero);
  // The previous frame was rendered with the pref value active at that
  // point; using the current pref value here is fine because a PATCH
  // toggle calls MarkDirty, which forces cell_diff_reset via the
  // validity-flip path so every cell repaints on the next render. A
  // scan-for-scan mismatch on a partial-refresh cycle would show a
  // stray leading zero for one frame — acceptable and self-healing.
  const ClockLayout before = ComputeClockLayout(
      prev_valid, prev_hour, prev_minute, kDigitPanels, hide_leading_zero);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — "dd/mm" date label. Repainted on first paint, date
  // rollover, or a full EPD refresh.
  slots[0] = PaintSlot{PaintSlot::kLabelSplit, label_text, nullptr, 0, 0};
  update[0] = cell_diff_reset || full_refresh_mode;

  // Digit panels 1..N-1 — HH:MM. ' ' pad cells short-circuit to no paint.
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                 std::string(1, now.digits[i]), nullptr, 0, 0};
    update[panel_idx] = cell_diff_reset || full_refresh_mode ||
                        now.digits[i] != before.digits[i];
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh_mode,
                  vertical_desc);
}

template void RenderClockScreen<7>(std::array<std::unique_ptr<EpdPanel>, 7>&,
                                   uint8_t (&)[7][16 * 296], const AppFonts&,
                                   bool, int, int, int, int, bool, int, int,
                                   int, int, bool, bool, bool);
template void RenderClockScreen<8>(std::array<std::unique_ptr<EpdPanel>, 8>&,
                                   uint8_t (&)[8][16 * 296], const AppFonts&,
                                   bool, int, int, int, int, bool, int, int,
                                   int, int, bool, bool, bool);

}  // namespace btclock
