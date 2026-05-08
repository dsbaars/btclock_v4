// Pure-logic decisions for the new-block event and the zap overlay.
//
// Small predicates, extracted here so host tests can pin the
// steal-focus + overlay-guard behaviour and the zap-overlay deadline
// arithmetic without pulling ScreenManager (which drags fonts + EPD +
// FreeRTOS into the test target).
//
// Wired into event_loop.cpp:
//   - when ConsumeNewBlock returns true the loop consults
//     ShouldSteal(pref, current_kind) before calling
//     ScreenManager::SetKind(kBlockHeight);
//   - when a zap lands the loop computes the overlay timeout via
//     ZapOverlayPolicy::ComputeTimeoutMs(timer_seconds) and passes it
//     through to ScreenManager::SetZapNotify.
// Keeping these rules here avoids spreading event policy between the
// event loop and the screen manager.

#pragma once

#include <cstdint>

#include "screens/screen_kind.hpp"

namespace btclock {

struct BlockEventPolicy {
  // Returns true when the main loop should jump the display to the
  // block-height screen in response to a new block. Rules:
  //   - steal_focus=false → never steal.
  //   - Already on kBlockHeight → no-op (the normal render path will
  //     refresh the digits).
  //   - kDebug + kNostrZap are respected: kDebug means the user is
  //     mid-debug and a yank would lose context; kNostrZap is a short-
  //     lived overlay with its own timeout that auto-restores the prior
  //     screen, so layering a steal underneath would skip the restore.
  //   - kCustom is NOT protected — a /api/show/text override is sticky
  //     until the next nav, but stealFocus is the user opting into
  //     "yes, please yank me off whatever I'm on when a block lands".
  //     A custom-text screen would otherwise stay up forever even
  //     though stealFocus is on.
  static constexpr bool ShouldSteal(bool steal_focus, ScreenType current) {
    if (!steal_focus) return false;
    switch (current) {
      case ScreenType::kBlockHeight:
      case ScreenType::kDebug:
      case ScreenType::kNostrZap:
        return false;
      default:
        return true;
    }
  }

  // Returns true when a non-stealing block update should still reset
  // the rotation deadline. Companion to ShouldSteal: covers the case
  // where the user is already on kBlockHeight and a new block arrives
  // — the steal path is a no-op, but the rotation timer keeps ticking
  // and could flip the screen seconds after the new digits paint
  // (matches the user's "saw the new height for one second" report).
  // Gated on `steal_focus` because that's the user opting into "the
  // new block is the most important thing on screen right now"; with
  // stealFocus off we keep the prior behaviour (rotation wins).
  // kDebug and kNostrZap are intentionally excluded — debug freezes
  // rotation on its own, and the zap overlay has its own deadline
  // arithmetic.
  static constexpr bool ShouldRestartTimerOnBlockUpdate(bool steal_focus,
                                                        ScreenType current) {
    if (!steal_focus) return false;
    return current == ScreenType::kBlockHeight;
  }

  // Threshold for the catch-up jump guard. Mirrors the v3 firmware's
  // hardcoded value (100 blocks ≈ 16 hours of chain advance) so a
  // device that boots after being offline for a few hours doesn't yank
  // the user, flash the LED, and pulse the frontlight as if 50 blocks
  // landed in the same minute.
  static constexpr uint32_t kCatchUpJumpBlocks = 100;

  // Returns true when the latest block-height update is almost
  // certainly the device catching up to chain tip rather than a
  // realtime new-block event. Suppresses the LED flash, frontlight
  // pulse, and stealFocus jump for the catch-up frame — the screen
  // still re-renders via the normal ShouldRender path so the new
  // height does appear.
  //
  // Rules:
  //   - prev_height == 0 → first observation (ScreenManager debounce
  //     also returns is_new=false here, so this path is never reached
  //     in practice; treat as not-catch-up to keep the predicate
  //     monotonic.)
  //   - new_height <= prev_height → reorg / no-op / stale read; never
  //     a catch-up (a real new block always increments).
  //   - new_height - prev_height > kCatchUpJumpBlocks → catch-up.
  static constexpr bool IsCatchUpJump(uint32_t prev_height,
                                      uint32_t new_height) {
    if (prev_height == 0) return false;
    if (new_height <= prev_height) return false;
    return (new_height - prev_height) > kCatchUpJumpBlocks;
  }
};

struct ZapOverlayPolicy {
  // Documented fallback when timerSeconds is unset / zero. Matches
  // ScreenManager::kZapTimeoutMs — duplicated here so the pure-logic
  // helper stays free of the ScreenManager header. A compile-time
  // link check sits in the ScreenManager TU (see CheckZapTimeoutParity
  // below).
  static constexpr int64_t kFallbackMs = 8'000;

  // Translate the `timerSeconds` NVS pref into the overlay's visible-
  // time window. Non-positive input (unset pref, or the user saved 0
  // by accident) falls back to the documented default so the zap
  // doesn't vanish before the viewer reads it.
  static constexpr int64_t ComputeTimeoutMs(int64_t timer_seconds) {
    if (timer_seconds <= 0) return kFallbackMs;
    return timer_seconds * 1000;
  }
};

}  // namespace btclock
