// Host tests for the pure-logic event-policy helpers extracted from
// event_loop.cpp. Covers:
//   - BlockEventPolicy::ShouldSteal     (Bug 1: new-block stealFocus)
//   - ZapOverlayPolicy::ComputeTimeoutMs (Bug 2: zap timeout from
//     timerSeconds pref)
//
// Both live as constexpr static methods so the compiler folds them at
// call sites — the tests also double-check the constexpr-ness via a
// static_assert block.

#include <initializer_list>

#include "app/block_event_policy.hpp"
#include "doctest.h"
#include "screens/screen_kind.hpp"

using btclock::BlockEventPolicy;
using btclock::ScreenType;
using btclock::ZapOverlayPolicy;

// --- BlockEventPolicy::ShouldSteal -----------------------------------

TEST_CASE(
    "ShouldSteal returns false when stealFocus=false regardless of current") {
  // Spot-check every enum value — none should steal when the pref is off.
  for (auto kind :
       {ScreenType::kBlockHeight, ScreenType::kMoscowTime,
        ScreenType::kBtcPrice, ScreenType::kBlockFeeRate, ScreenType::kClock,
        ScreenType::kHalving, ScreenType::kBitcoinSupply,
        ScreenType::kMarketCap, ScreenType::kMiningPoolHashrate,
        ScreenType::kMiningPoolEarnings, ScreenType::kBitaxeHashrate,
        ScreenType::kBitaxeBestDiff, ScreenType::kCustom, ScreenType::kDebug,
        ScreenType::kNostrZap}) {
    CHECK_FALSE(BlockEventPolicy::ShouldSteal(false, kind));
  }
}

TEST_CASE(
    "ShouldSteal returns true on a normal data screen when stealFocus=true") {
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kMoscowTime));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kBtcPrice));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kBlockFeeRate));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kClock));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kHalving));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kBitcoinSupply));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kMarketCap));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kMiningPoolHashrate));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kMiningPoolEarnings));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kBitaxeHashrate));
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kBitaxeBestDiff));
}

TEST_CASE("ShouldSteal is a no-op when already on kBlockHeight") {
  // Already-on means the next Render() will pick up the new digit via
  // the normal snapshot-diff path, no nav needed.
  CHECK_FALSE(BlockEventPolicy::ShouldSteal(true, ScreenType::kBlockHeight));
}

TEST_CASE("ShouldSteal respects debug + zap overlays but not custom") {
  // kDebug: user is mid-debug, yanking would lose state.
  CHECK_FALSE(BlockEventPolicy::ShouldSteal(true, ScreenType::kDebug));
  // kNostrZap: transient overlay with its own timeout that auto-
  // restores the prior screen — layering a steal underneath would
  // skip the restore step.
  CHECK_FALSE(BlockEventPolicy::ShouldSteal(true, ScreenType::kNostrZap));
  // kCustom: a /api/show/text override is sticky until next nav. With
  // stealFocus=true the user explicitly opted into "yank me to the
  // block-height screen on new blocks", so the custom text yields.
  CHECK(BlockEventPolicy::ShouldSteal(true, ScreenType::kCustom));
}

// --- ZapOverlayPolicy::ComputeTimeoutMs ------------------------------

TEST_CASE("ComputeTimeoutMs multiplies timerSeconds into ms") {
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(10) == 10'000);
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(30) == 30'000);
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(1) == 1'000);
}

TEST_CASE("ComputeTimeoutMs falls back on non-positive input") {
  // NVS default / unset / user-saved-zero: clamp to documented
  // fallback so the overlay doesn't vanish before the viewer reads it.
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(0) == ZapOverlayPolicy::kFallbackMs);
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(-1) ==
        ZapOverlayPolicy::kFallbackMs);
  CHECK(ZapOverlayPolicy::ComputeTimeoutMs(-1'000) ==
        ZapOverlayPolicy::kFallbackMs);
}

// Lock in the documented fallback value — drift from 8s would surprise
// users whose settings only had timerSeconds=0 before this fix.
TEST_CASE("ZapOverlayPolicy::kFallbackMs matches the documented 8 s window") {
  CHECK(ZapOverlayPolicy::kFallbackMs == 8'000);
}

// Compile-time assertions keep the helpers usable in constant-expression
// contexts — the event loop constructs the timeout inline, and the
// steal check feeds a branch predictor target.
static_assert(BlockEventPolicy::ShouldSteal(true, ScreenType::kMoscowTime),
              "ShouldSteal not constexpr-callable");
static_assert(!BlockEventPolicy::ShouldSteal(true, ScreenType::kBlockHeight),
              "ShouldSteal must reject kBlockHeight");
static_assert(ZapOverlayPolicy::ComputeTimeoutMs(15) == 15'000,
              "ComputeTimeoutMs not constexpr-callable");
static_assert(ZapOverlayPolicy::ComputeTimeoutMs(0) ==
                  ZapOverlayPolicy::kFallbackMs,
              "ComputeTimeoutMs fallback drifted");
