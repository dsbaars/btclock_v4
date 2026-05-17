#include "refresh_policy/refresh_policy.hpp"

namespace btclock {

bool RefreshPolicy::Decide(RefreshPolicyState& state, int64_t now_ms,
                           bool is_screen_change, bool is_force_full,
                           bool refr_scrn_change, int full_refresh_min) {
  // Precedence order mirrors the decision tree in the brief:
  //
  //   1. is_force_full — unconditional. Covers first-render
  //      (dirty_=true on construction), /api/full_refresh, the
  //      inverted_color PATCH repaint, and the debug-overlay exit
  //      path. Same override used by the old firmware's epd.cpp
  //      "force full" callers.
  //   2. Screen change + refr_scrn_change=true — user picked the
  //      "full on every nav" mode. Matches v3
  //      screen_handler.cpp:21's getBool("refrScrnChange", ...)
  //      gate.
  //   3. Screen change + fullRefreshMin elapsed — periodic ghost-
  //      clear schedule, also cleared on any full refresh (so a
  //      settings change resets the window).
  //   4. Anything else → partial. Digit-flip re-paints on the same
  //      slot fall here; they only repaint the cells that changed.
  // Hint: by far the most frequent caller of Decide() is the
  // same-screen data-update path (digit flip every minute, hashprice
  // tick, mempool blocks bump) where every input flag is false and
  // we fall straight through to "partial". The full-refresh paths
  // (force-full, screen change with refrScrnChange=true, or the
  // periodic ghost-clear schedule) all run orders of magnitude less
  // often. Marking those as [[unlikely]] lets the compiler keep the
  // straight-through path as the hot fall-through with the full
  // branches cold. Measured against this codebase the hint is
  // size-neutral at -Os (0 B Rev A delta) — kept as documentation
  // and a perf hedge for future call-site growth.
  bool full = false;
  if (is_force_full) [[unlikely]] {
    full = true;
  } else if (is_screen_change) [[unlikely]] {
    if (refr_scrn_change) {
      full = true;
    } else {
      const bool never =
          state.last_full_refresh_ms == RefreshPolicyState::kNever;
      // fullRefreshMin<=0 degrades to "always full on screen change"
      // rather than "never full": the old firmware's epd.cpp uses the
      // same minutes-to-ms multiplication which lands on 0 here, and a
      // 0-ms "elapsed since last full" deadline is always satisfied.
      // Treating the pref as always-full at 0 preserves that semantic
      // without pretending 0 means "disable full refreshes" (which
      // would accumulate ghosting forever).
      const int64_t threshold_ms =
          static_cast<int64_t>(full_refresh_min) * 60LL * 1000LL;
      const int64_t elapsed = now_ms - state.last_full_refresh_ms;
      full = never || elapsed >= threshold_ms;
    }
  }
  if (full) [[unlikely]]
    state.last_full_refresh_ms = now_ms;
  return full;
}

}  // namespace btclock
