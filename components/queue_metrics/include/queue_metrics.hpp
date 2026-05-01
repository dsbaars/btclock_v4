// Per-queue drop counters.
//
// Producers (buttons, led_controller, frontlight_controller) use
// `xQueueSend(..., 0)` so a wedged consumer cannot stall the producing
// task. The downside is that a backed-up queue silently eats events.
// This header gives every producer a one-call hook to bump a counter
// on a failed send so the WebUI / `/api/system_status` can surface
// drops as a field-debug signal. Counters are monotonic since boot
// and never reset (uint32 wraps after ~136 years at 1 drop/s).
//
// The storage is std::atomic<uint32_t>: producers may call from any
// task (the buttons poller is a regular task; LED + frontlight Post()
// runs on whoever calls them — main, HTTP worker, BH1750 sampler).
// memory_order_relaxed is sufficient — the counter is observational,
// not a synchronisation primitive.

#pragma once

#include <cstdint>

namespace btclock {
namespace queue_metrics {

enum class Queue : std::uint8_t {
  kButtons = 0,
  kLed,
  kFrontlight,
  kMax,  // sentinel — keep last
};

// Bump the drop counter for `q`. Safe to call from any task. No-op if
// `q` is out of range (defensive — callers should pass a valid enum).
void RecordDrop(Queue q);

// Read the current drop count for `q`. Returns 0 if `q` is out of
// range.
std::uint32_t GetDrops(Queue q);

}  // namespace queue_metrics
}  // namespace btclock
