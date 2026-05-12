// Bounded MPSC-style queue for NWC payment notifications.
//
// The on-device hot path lives on the esp_websocket_client RX-callback
// task (~3-4 KiB stack). NIP-44 v2 decrypt + cJSON decode of a
// kind-23197 notification blows past that budget and crashes the
// device on first notification arrival. Same pattern as the earlier
// esp_timer overflow we fixed for the get_balance poll.
//
// Fix: the WS task only enqueues the encrypted payload + a tiny
// envelope. A dedicated worker task with an 8 KiB stack drains the
// queue and runs the heavy decrypt/decode/dispatch path. The hot path
// stays bounded and microsecond-scale.
//
// Capacity is intentionally small (8 slots × ~1 KiB worst-case = ~8 KiB
// ceiling). Notifications are conversational events; bursts > 8 in
// 250 ms are non-physical. On overflow we drop the new item and bump
// `dropped()`, so the operator can see the saturation in
// /api/nwc/debug.
//
// Threading: `TryPush` and `WaitPop` are safe to call concurrently
// from any task; the queue serialises via std::mutex. This file uses
// portable std primitives (mutex + condvar + deque) — ESP-IDF v6 ships
// libstdc++ with FreeRTOS-pthread backing, so the same TU compiles on
// both target and host.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace btclock {
namespace nwc {

// Minimal envelope copied off the WS task. The encrypted payload
// (NIP-44 v2 base64 ≤ 1 KiB or NIP-04 base64 + iv) is the heaviest
// field. `event_id` is kept for log/dedup; could be dropped if we ever
// need to shave bytes.
struct RawNotification {
  uint32_t kind = 0;     // 23197 (modern) or 23196 (legacy)
  std::string content;   // base64 ciphertext, ≤ ~1 KiB
  std::string event_id;  // 64-char hex, optional
};

class NotificationQueue {
 public:
  // 8 slots matches the architecture budget — see file header.
  static constexpr size_t kDefaultCapacity = 8;

  explicit NotificationQueue(size_t capacity = kDefaultCapacity);
  ~NotificationQueue();

  NotificationQueue(const NotificationQueue&) = delete;
  NotificationQueue& operator=(const NotificationQueue&) = delete;

  // Non-blocking push. Returns true if accepted; false (and bumps the
  // drop counter) if the queue is full or has been shut down.
  bool TryPush(RawNotification item);

  // Block up to `timeout_ms` waiting for an item. Returns true on
  // success, false on timeout or shutdown. On success `out` is move-
  // assigned the popped item.
  bool WaitPop(RawNotification& out, uint32_t timeout_ms);

  // Wake any blocked waiter; subsequent TryPush calls fail. Idempotent.
  void Shutdown();

  size_t capacity() const { return capacity_; }
  size_t size() const;
  uint32_t pushed() const { return pushed_.load(); }
  uint32_t popped() const { return popped_.load(); }
  uint32_t dropped() const { return dropped_.load(); }

 private:
  const size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<RawNotification> items_;
  bool shutdown_ = false;
  std::atomic<uint32_t> pushed_{0};
  std::atomic<uint32_t> popped_{0};
  std::atomic<uint32_t> dropped_{0};
};

}  // namespace nwc
}  // namespace btclock
