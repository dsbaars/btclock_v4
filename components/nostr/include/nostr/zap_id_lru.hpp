// Bounded zap-event LRU for multi-relay deduplication.
//
// Three relays publishing the same NIP-57 receipt fire MarkFresh()
// three times in quick succession from independent websocket worker
// tasks. The first call returns true (caller proceeds with the zap
// notification — screen overlay, LED flash, frontlight pulse); the
// next two return false so the user sees one notification per zap,
// not three.
//
// Why FIFO and not true LRU: zap arrival rate is slow enough (typically
// seconds-to-minutes apart) that the eviction discipline doesn't matter
// for correctness — by the time the deque rolls over, every still-
// duplicating relay has already delivered. FIFO is one mutex-protected
// std::deque + std::unordered_set, no extra accounting.
//
// Memory budget: 64 entries × (64-char hex string + set + deque
// overhead) ≈ 5 KB heap. Bounded so a noisy relay can't grow the set
// unbounded.

#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace btclock {
namespace nostr {

class ZapIdLru {
 public:
  // Capacity sized to "more than any plausible burst across N relays
  // for the same zap". 64 leaves headroom even when all four allowed
  // relays redeliver an in-flight invoice on reconnect.
  static constexpr std::size_t kCap = 64;

  // Returns true when this is the first sight of `event_id` (caller
  // should fire the notification), false when it's already in the LRU
  // (caller should drop the duplicate). An empty `event_id` is treated
  // as fresh — the caller can always opt out of dedup for events
  // missing an id by passing "".
  bool MarkFresh(std::string_view event_id) {
    if (event_id.empty()) return true;
    std::lock_guard<std::mutex> guard(mu_);
    std::string key(event_id);
    if (set_.count(key) != 0) return false;
    if (deque_.size() >= kCap) {
      set_.erase(deque_.front());
      deque_.pop_front();
    }
    deque_.push_back(key);
    set_.insert(std::move(key));
    return true;
  }

 private:
  std::mutex mu_;
  std::deque<std::string> deque_;
  std::unordered_set<std::string> set_;
};

}  // namespace nostr
}  // namespace btclock
