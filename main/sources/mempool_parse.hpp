// Pure parse helpers split out of mempool_kraken_source.cpp so host tests
// can pin mempool.space's wire shape without dragging in ESP-IDF /
// esp_websocket_client. Same rationale as sources_uri.cpp.

#pragma once

#include <cstdint>

#include "cJSON.h"

namespace btclock {

// Extract the chain tip from mempool.space's `blocks` array.
//
// The initial snapshot (sent right after the `want: [blocks, ...]`
// subscribe) carries the last 8 confirmed blocks in ASCENDING height
// order — blocks[0] is the OLDEST, the tip is the LAST element. Verified
// live against wss://mempool.space/api/v1/ws on 2026-08-02:
//
//   heights: [960719, 960720, 960721, 960722, 960723, 960724, 960725, 960726]
//
// Reading index 0 (the pre-fix behaviour) therefore reported a height up
// to 7 blocks stale on every boot and reconnect, self-correcting only
// when the next `block` (singular) push landed ~10 minutes later.
//
// We scan for the maximum rather than blindly taking the last element:
// the tip is the tip regardless of how the upstream chooses to order the
// array, so a future ordering flip can't silently reintroduce the bug.
//
// Returns false and leaves *out untouched when `blocks` is not an array,
// is empty, or contains no usable numeric `height`.
bool TipHeightFromBlocksArray(const cJSON* blocks, std::uint32_t* out);

}  // namespace btclock
