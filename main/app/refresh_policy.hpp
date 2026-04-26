// EPD refresh policy — decides full vs partial for the next paint.
//
// Pulled out of ScreenManager so the state machine can be pinned by
// host tests without dragging in the EPD driver, fonts bundle, or
// FreeRTOS. The decision combines three inputs: the user's
// `refrScrnChange` pref (force full on every screen change),
// `fullRefreshMin` (mandatory full-refresh interval in minutes), and
// the "force full" override used by MarkDirty() / first-render /
// navigation + debug-exit.
//
// State is tiny: the monotonic-ms timestamp of the last full refresh.
// Callers construct a RefreshPolicyState alongside their ScreenManager
// and hand it + the four boolean/int inputs to Decide(). When Decide()
// returns true, the caller paints a full refresh AND the policy's
// `last_full_refresh_ms` has already been stamped to `now_ms` — a
// subsequent settings-change-driven full refresh will restart the
// timer too, matching the old firmware's epd.cpp semantics.

#pragma once

#include <cstdint>

namespace btclock {

struct RefreshPolicyState {
  // Monotonic ms timestamp of the last full refresh. `kNever` (<0)
  // encodes "no full refresh has happened yet" so the first paint
  // always comes out full regardless of the fullRefreshMin window.
  static constexpr int64_t kNever = -1;
  int64_t last_full_refresh_ms = kNever;
};

struct RefreshPolicy {
  // Inputs:
  //   now_ms            — current monotonic timestamp (ms).
  //   is_screen_change  — true when the caller is painting as a
  //                       direct consequence of a slot/currency
  //                       navigation (Next/Prev/SetSlot/SetCurrency/
  //                       auto-rotate). Same-screen data pushes pass
  //                       false.
  //   is_force_full     — true when the caller explicitly demands a
  //                       full refresh this paint (MarkDirty() from
  //                       /api/full_refresh, invertedColor PATCH,
  //                       debug-overlay exit, first render).
  //   refr_scrn_change  — user pref: force full on every screen
  //                       change.
  //   full_refresh_min  — user pref: mandatory full refresh after
  //                       this many minutes. 0 means "always full on
  //                       screen change" (same effect as
  //                       refr_scrn_change=true for the screen-change
  //                       path; same-screen paints still partial).
  //
  // Returns true iff the caller should paint a full refresh. When the
  // return is true, `state.last_full_refresh_ms` has been stamped to
  // `now_ms` — next call re-starts its timer from the same point,
  // so a settings-change full and a schedule-driven full can't
  // fire back-to-back on the following tick.
  static bool Decide(RefreshPolicyState& state, int64_t now_ms,
                     bool is_screen_change, bool is_force_full,
                     bool refr_scrn_change, int full_refresh_min);
};

}  // namespace btclock
