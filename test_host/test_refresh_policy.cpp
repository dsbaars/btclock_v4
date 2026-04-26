// Host tests for the EPD refresh policy — full-vs-partial decision
// extracted from ScreenManager::Render so the state machine can be
// pinned without the EPD driver / fonts / FreeRTOS surface.
//
// The cases below exercise the three user-visible inputs
// (refrScrnChange, fullRefreshMin, force-full override) plus the
// implicit first-render rule, in isolation from the rotation and
// render pipeline. Matches the contract specified by the bug brief
// ("fix full-vs-partial EPD refresh cadence").

#include "app/refresh_policy.hpp"
#include "doctest.h"

using btclock::RefreshPolicy;
using btclock::RefreshPolicyState;

namespace {
// Minutes→ms helper — makes the intent visible at the call sites and
// matches the units the user-facing pref is surfaced in (minutes).
constexpr int64_t kMin = 60LL * 1000LL;
}  // namespace

TEST_CASE("first render is full regardless of prefs") {
  RefreshPolicyState s;
  // Any combination of flags; the "never happened" seed forces full.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/0,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 0);
}

TEST_CASE("first render without a screen change is partial") {
  // An unrelated snapshot push on the initial slot shouldn't burn a
  // full refresh — only nav / force-full / schedule trigger a full.
  RefreshPolicyState s;
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/0,
                                    /*is_screen_change=*/false,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == RefreshPolicyState::kNever);
}

TEST_CASE("refrScrnChange=true forces full on every screen change") {
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;  // warm seed
  // Partial-window: only 1 minute elapsed, but user wants full.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/1 * kMin,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/true,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 1 * kMin);
  // Next screen change at 2 min — still full.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/2 * kMin,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/true,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 2 * kMin);
}

TEST_CASE("refrScrnChange=false: screen change within window is partial") {
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // 5 minutes later, well under the 10-min threshold.
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/5 * kMin,
                                    /*is_screen_change=*/true,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/10));
  // Timer untouched — partial paints don't reset the full-refresh
  // window, otherwise a user rapidly clicking next would forever
  // postpone the mandatory ghost-clear.
  CHECK(s.last_full_refresh_ms == 0);
}

TEST_CASE(
    "refrScrnChange=false: screen change past window is full and timer "
    "resets") {
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // Exactly at the threshold — the "≥" compare should fire full.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/10 * kMin,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 10 * kMin);
  // Immediately after — back to partial (timer just reset).
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/10 * kMin + 1,
                                    /*is_screen_change=*/true,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 10 * kMin);
}

TEST_CASE("is_force_full overrides everything") {
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // Same-screen data push with the force-full override (e.g.
  // invertedColor PATCH -> MarkDirty()). Screen change = false so
  // neither refrScrnChange nor fullRefreshMin path gates fire — only
  // is_force_full does.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/1 * kMin,
                              /*is_screen_change=*/false,
                              /*is_force_full=*/true,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 1 * kMin);
}

TEST_CASE("force-full resets the fullRefreshMin window") {
  // Covers the brief: "The fullRefreshMin timer should restart on any
  // full refresh (whether driven by the schedule or by a settings
  // change), so we don't fire back-to-back fulls."
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // Force-full at 5 min.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/5 * kMin,
                              /*is_screen_change=*/false,
                              /*is_force_full=*/true,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 5 * kMin);
  // Screen change at 12 min — 7 min after the last full (< 10 min
  // window). Should be partial because the force-full reset the
  // clock.
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/12 * kMin,
                                    /*is_screen_change=*/true,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/10));
}

TEST_CASE("same-screen data update is always partial") {
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // Simulate a digit flip hours after the last full — still partial
  // because it's not a screen change and not force-full. The
  // fullRefreshMin window only gets cleared at nav events; a
  // continuously-mounted slot never triggers a scheduled full on its
  // own. (Matches the old firmware: the schedule check sits inside
  // the display-update path, but fires on the next renderable event
  // rather than synthesising a wake-up.)
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/60 * kMin,
                                    /*is_screen_change=*/false,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 0);
}

TEST_CASE("fullRefreshMin=0 degrades to full on every screen change") {
  // Edge case called out in the brief: fullRefreshMin=0 should
  // behave like refrScrnChange=true for the nav path. Same-screen
  // data pushes stay partial.
  RefreshPolicyState s;
  s.last_full_refresh_ms = 0;
  // Navigation — full (elapsed >= 0 always true).
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/1,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/0));
  CHECK(s.last_full_refresh_ms == 1);
  // Same-screen refresh — still partial regardless of 0 threshold.
  CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/2,
                                    /*is_screen_change=*/false,
                                    /*is_force_full=*/false,
                                    /*refr_scrn_change=*/false,
                                    /*full_refresh_min=*/0));
}

TEST_CASE("default fullRefreshMin=60 honors partial rotation for <60 min") {
  // btclock_v4-jo6 user-facing scenario: factory defaults
  // (refrScrnChange=false, fullRefreshMin=60) mean a user rotating
  // through screens every few seconds should see partial refreshes,
  // not a full-every-tick flash. After the initial full refresh lands
  // at t=0, a burst of rotation ticks within the 60-min window stays
  // on the partial path.
  RefreshPolicyState s;
  // First screen-change = full regardless of threshold (kNever seed).
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/0,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/60));
  CHECK(s.last_full_refresh_ms == 0);
  // Simulate 30 rotation ticks spaced 10 s apart. None should force
  // full — every last-rendered ms < 60 min later.
  for (int i = 1; i <= 30; ++i) {
    const int64_t t = static_cast<int64_t>(i) * 10 * 1000LL;  // 10 s steps
    CHECK_FALSE(RefreshPolicy::Decide(s, t,
                                      /*is_screen_change=*/true,
                                      /*is_force_full=*/false,
                                      /*refr_scrn_change=*/false,
                                      /*full_refresh_min=*/60));
  }
  // Window still open after 5 minutes.
  CHECK(s.last_full_refresh_ms == 0);
  // Cross the 60-min boundary — next transition should be full.
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/60 * kMin,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/60));
  CHECK(s.last_full_refresh_ms == 60 * kMin);
}

TEST_CASE(
    "scheduled full fires even if refrScrnChange=false and only one nav a "
    "session") {
  // Long-running device that's been on slot X for an hour: the first
  // screen change at that point must produce a full refresh to clear
  // accumulated ghosting.
  RefreshPolicyState s;
  // Seed with a full refresh having happened at boot time (t=0).
  s.last_full_refresh_ms = 0;
  // 59 minutes of same-screen data updates. None of them mutate the
  // state — partials all the way.
  for (int t = 1; t < 60; ++t) {
    CHECK_FALSE(RefreshPolicy::Decide(s, /*now_ms=*/t * kMin,
                                      /*is_screen_change=*/false,
                                      /*is_force_full=*/false,
                                      /*refr_scrn_change=*/false,
                                      /*full_refresh_min=*/10));
  }
  CHECK(s.last_full_refresh_ms == 0);
  // At t=60min the user presses Next → full refresh (elapsed 60 ≥ 10).
  CHECK(RefreshPolicy::Decide(s, /*now_ms=*/60 * kMin,
                              /*is_screen_change=*/true,
                              /*is_force_full=*/false,
                              /*refr_scrn_change=*/false,
                              /*full_refresh_min=*/10));
  CHECK(s.last_full_refresh_ms == 60 * kMin);
}
