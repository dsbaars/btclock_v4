// Host tests for the BlockHeightValidator predicate that gates which
// block-height updates flow into the canonical hub snapshot.
//
// Mirrors v3 commit b435552 ("fix: verify block update"). Without this
// guard, a corrupt source frame (height=0, regression below current
// tip) reaches ConsumeNewBlock in event_loop.cpp and triggers the full
// new-block reaction (LED purple-flash, frontlight pulse, stealFocus
// yank). The wild-jump catch-up case (legit ≥100-block deltas after
// a long offline window) is intentionally NOT rejected here — that's
// BlockEventPolicy::IsCatchUpJump's job at the event-loop layer, and
// has its own test_block_event_policy.cpp coverage.

#include <cstdint>

#include "data_core/block_height_validator.hpp"
#include "doctest.h"

using btclock::BlockHeightValidator;

TEST_CASE("IsValidUpdate: cold snapshot (prev=0) accepts any non-zero new") {
  // First observation seeds the tracker — the source's first frame
  // sets the canonical height regardless of magnitude.
  CHECK(BlockHeightValidator::IsValidUpdate(0, 1));
  CHECK(BlockHeightValidator::IsValidUpdate(0, 900'000));
  CHECK(BlockHeightValidator::IsValidUpdate(0, 4'000'000'000u));
}

TEST_CASE("IsValidUpdate: cold snapshot rejects zero (corrupt sentinel)") {
  // height=0 from an empty source struct or a malformed frame must not
  // seed the tracker — otherwise a real ≥1 update later would read as
  // a regression against zero (vacuously true) and pass through.
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(0, 0));
}

TEST_CASE("IsValidUpdate: zero new always rejected, regardless of prev") {
  // Chain tip is never literal zero post-genesis. A live source emitting
  // 0 is always a corrupt frame.
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(900'000, 0));
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(1, 0));
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(4'000'000'000u, 0));
}

TEST_CASE("IsValidUpdate: live update (new > prev) accepted") {
  // Normal new-block path — every minute or so under live conditions.
  CHECK(BlockHeightValidator::IsValidUpdate(900'000, 900'001));
  CHECK(BlockHeightValidator::IsValidUpdate(900'000, 900'010));
  // Catch-up after offline (>100 blocks) is still accepted at this
  // layer; the event-loop's IsCatchUpJump suppresses the side
  // effects but the snapshot still holds the new tip.
  CHECK(BlockHeightValidator::IsValidUpdate(900'000, 901'000));
  CHECK(BlockHeightValidator::IsValidUpdate(900'000, 1'000'000));
}

TEST_CASE("IsValidUpdate: equal value accepted (caller dedupes)") {
  // The validator's contract is "is this update sane?", not "is it
  // novel?". `DataSnapshot::Merge` already short-circuits on
  // `*block_height == *other.block_height`, so the equal case never
  // calls the validator — but an explicit accept here documents the
  // boundary so a future caller can't trip on it.
  CHECK(BlockHeightValidator::IsValidUpdate(900'000, 900'000));
}

TEST_CASE("IsValidUpdate: regression (new < prev) rejected") {
  // Real reorgs unwind ≤6 blocks but the source re-emits the new tip
  // on its own, not a backwards step — so a strictly-lower height is
  // either a corrupt frame, a buggy source, or a reorg deeper than
  // we'd act on anyway. Reject across the depth spectrum.
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(900'000, 899'999));
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(900'000, 899'994));
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(900'000, 800'000));
  CHECK_FALSE(BlockHeightValidator::IsValidUpdate(900'000, 1));
}

TEST_CASE("IsValidUpdate: constexpr-evaluable") {
  // Folds at the call site so the gate is two compares and a branch.
  static_assert(BlockHeightValidator::IsValidUpdate(900'000, 900'001));
  static_assert(!BlockHeightValidator::IsValidUpdate(900'000, 0));
  static_assert(!BlockHeightValidator::IsValidUpdate(900'000, 899'999));
  static_assert(BlockHeightValidator::IsValidUpdate(0, 900'000));
  static_assert(!BlockHeightValidator::IsValidUpdate(0, 0));
  CHECK(true);  // doctest needs an assertion in the body.
}
