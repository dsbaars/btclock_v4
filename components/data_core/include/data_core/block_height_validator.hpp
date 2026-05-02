// Pure-logic guard for block-height updates flowing through the hub.
//
// A WebSocket frame from any of the data sources (mempool.space, the
// btclock v2 endpoint, nostr) lands as a `DataSnapshot::block_height`
// value that the hub merges into its canonical snapshot. Without a
// validator, a corrupt frame (height=0, height regressing below the
// previous tip) drove the new-block reaction in event_loop.cpp:
//
//   - LED purple-flash (kBlockFlash)
//   - Frontlight pulse
//   - stealFocus yank to the block-height screen
//
// Mirrors v3 commit b435552 ("fix: verify block update") which guarded
// processNewBlock with a `new <= prev` early-return. The wild-jump
// catch-up regime (legitimate ≥100-block deltas after a long offline
// window) is handled separately in `BlockEventPolicy::IsCatchUpJump`
// at the event-loop layer — the validator's job here is *only* to
// keep obviously-corrupt frames out of the canonical snapshot.
//
// Lives in `data_core` rather than `main/app` so `DataSnapshot::Merge`
// can call it without inverting the dependency graph.

#pragma once

#include <cstdint>

namespace btclock {

struct BlockHeightValidator {
  // Returns true when `new_height` should replace `prev_height` in the
  // canonical snapshot. Rules:
  //   - `new_height == 0` → reject (corrupt sentinel; chain tip is
  //     never zero post-genesis and a real source would emit nothing
  //     rather than a literal zero).
  //   - `prev_height == 0` → accept (cold snapshot; the very first
  //     observation seeds the tracker).
  //   - `new_height < prev_height` → reject (regression / corrupt
  //     frame; legitimate reorgs unwind ≤6 blocks but the source
  //     should re-emit the new tip on its own, not a backwards step).
  //   - `new_height >= prev_height` → accept (live update or no-op;
  //     the caller deduplicates `==` already).
  static constexpr bool IsValidUpdate(uint32_t prev_height,
                                      uint32_t new_height) {
    if (new_height == 0) return false;
    if (prev_height == 0) return true;
    return new_height >= prev_height;
  }
};

}  // namespace btclock
