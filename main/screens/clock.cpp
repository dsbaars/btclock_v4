#include "screens/screens.hpp"

#include <cstdio>

#include "screens/common.hpp"

namespace btclock {

// Panel 0 = dd/mm date (split-text, matches the old firmware's
// `std::to_string(mday)+"/"+std::to_string(mon+1)` payload that
// EPDManager then routes through splitText on the '/' separator).
// Panels 1..N-1 = HH:MM digits right-justified, with ':' in the
// slot between HH and MM. If wall-clock isn't yet plausible
// (tv_sec predates 2020 — SNTP still pending) the time panels are
// blanked rather than showing an epoch glitch.

namespace {
// '0'..'9' use kDigitRef; ':' needs its own ref because its "ink"
// spans only two small dots vertically — falling back to kDigitRef
// there would inherit the digit above/below_baseline but DrawText
// only emits two pixels of ink, centered. That's what we want: the
// colon floats between the two digit rows at the digit baseline.
void DrawOne(LandscapeFb& lfb, const AppFonts& fonts, char c) {
  if (c == ' ') return;
  const char one[2] = {c, '\0'};
  DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                   kDigitRef, fonts.antonio(), 180.0f,
                   /*white_text=*/false);
}
}  // namespace

template <size_t N>
void RenderClockScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    bool valid, int hour, int minute, int mday, int month,
    bool prev_valid, int prev_hour, int prev_minute,
    int prev_mday, int prev_month) {
  static_assert(N >= 7, "clock layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  // First paint (no prior state) OR date rolled over OR validity flipped.
  const bool date_changed =
      !prev_valid || prev_mday != mday || prev_month != month;
  const bool full_refresh =
      (!prev_valid && valid) || date_changed;

  // Panel 0 — date label "dd/mm" via DrawSplitText. When time isn't
  // yet plausible we draw an em-dash split to tell the user the
  // screen exists but NTP hasn't landed yet.
  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    char top[4];
    char bot[4];
    if (valid) {
      std::snprintf(top, sizeof(top), "%d", mday);
      std::snprintf(bot, sizeof(bot), "%d", month);
    } else {
      std::snprintf(top, sizeof(top), "-");
      std::snprintf(bot, sizeof(bot), "-");
    }
    // Inherit the digit font so the WASM preview's swappable antonio
    // slot carries the date split-text too (Bug 1 — see block_height).
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, top, bot,
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.antonio(), 54.0f, /*white_text=*/false);
  }

  const ClockLayout now =
      ComputeClockLayout(valid, hour, minute, kDigitPanels);
  const ClockLayout before =
      ComputeClockLayout(prev_valid, prev_hour, prev_minute, kDigitPanels);

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh || now.digits[i] != before.digits[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    DrawOne(lfb, fonts, now.digits[i]);
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

template void RenderClockScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, bool, int, int, int, int, bool, int, int, int, int);
template void RenderClockScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, bool, int, int, int, int, bool, int, int, int, int);

}  // namespace btclock
