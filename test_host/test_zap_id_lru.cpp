// Host tests for ZapIdLru — multi-relay event-id dedup.
//
// The ZapIdLru sits between every ZapListener's relay-worker callback
// and the on-zap notification dispatch (snapshot patch + screen-overlay
// flag). When two sibling relays deliver the same kind-9735 receipt
// the first MarkFresh() call wins and the rest drop, so the user sees
// one notification per zap regardless of relay count.

#include <string>
#include <vector>

#include "doctest.h"
#include "nostr/zap_id_lru.hpp"

TEST_CASE(
    "ZapIdLru: first sight returns true, immediate replay returns false") {
  btclock::nostr::ZapIdLru lru;
  CHECK(lru.MarkFresh("event-id-1"));
  CHECK_FALSE(lru.MarkFresh("event-id-1"));
}

TEST_CASE("ZapIdLru: distinct ids are independent") {
  btclock::nostr::ZapIdLru lru;
  CHECK(lru.MarkFresh("event-id-1"));
  CHECK(lru.MarkFresh("event-id-2"));
  CHECK(lru.MarkFresh("event-id-3"));
  // Each is now seen — replays drop.
  CHECK_FALSE(lru.MarkFresh("event-id-1"));
  CHECK_FALSE(lru.MarkFresh("event-id-2"));
  CHECK_FALSE(lru.MarkFresh("event-id-3"));
}

TEST_CASE("ZapIdLru: empty event_id always treats as fresh") {
  // A relay misbehaving and dropping the id field shouldn't suppress
  // every subsequent zap — fall through and let the per-listener gate
  // handle it.
  btclock::nostr::ZapIdLru lru;
  CHECK(lru.MarkFresh(""));
  CHECK(lru.MarkFresh(""));
}

TEST_CASE("ZapIdLru: FIFO eviction past kCap allows old id to fire again") {
  // The cap is small enough that filling it 1+ times is cheap. After
  // pushing kCap distinct ids and then one more, the oldest id (id-0)
  // is evicted and re-marking it returns true again. This is correct
  // behaviour: a zap arriving 64 zaps later from a stuck relay cache
  // is plausibly a real "new" notification, and we'd rather over-fire
  // than indefinitely retain every id we've ever seen (memory cap).
  btclock::nostr::ZapIdLru lru;
  for (std::size_t i = 0; i < btclock::nostr::ZapIdLru::kCap; ++i) {
    CHECK(lru.MarkFresh("id-" + std::to_string(i)));
  }
  // One past cap → evicts id-0.
  CHECK(lru.MarkFresh("id-overflow"));
  // id-0 is evicted, so its next sight is "fresh" again.
  CHECK(lru.MarkFresh("id-0"));
  // The most-recently-seen ids are still in the LRU.
  CHECK_FALSE(lru.MarkFresh("id-overflow"));
  CHECK_FALSE(lru.MarkFresh(
      "id-" + std::to_string(btclock::nostr::ZapIdLru::kCap - 1)));
}

TEST_CASE("ZapIdLru: cap is bounded — never grows past kCap entries") {
  // Probe via a no-op sequence: insert kCap*3 distinct ids; only the
  // most recent kCap should still fingerprint as "seen". A leak past
  // kCap would leave older ids dropping when they should have been
  // evicted. The invariant matters because zap event ids are 64-char
  // hex and each entry costs ~80 B once set/deque overhead is counted.
  btclock::nostr::ZapIdLru lru;
  const std::size_t total = btclock::nostr::ZapIdLru::kCap * 3;
  for (std::size_t i = 0; i < total; ++i) {
    lru.MarkFresh("id-" + std::to_string(i));
  }
  // The earliest 2*kCap should all be evicted by now.
  for (std::size_t i = 0; i < total - btclock::nostr::ZapIdLru::kCap; ++i) {
    CAPTURE(i);
    CHECK(lru.MarkFresh("id-" + std::to_string(i)));
  }
}
