#include "queue_metrics.hpp"

#include <atomic>
#include <cstddef>

namespace btclock {
namespace queue_metrics {
namespace {

constexpr std::size_t kCount = static_cast<std::size_t>(Queue::kMax);
std::atomic<std::uint32_t> g_drops[kCount];

}  // namespace

void RecordDrop(Queue q) {
  const auto idx = static_cast<std::size_t>(q);
  if (idx >= kCount) return;
  g_drops[idx].fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t GetDrops(Queue q) {
  const auto idx = static_cast<std::size_t>(q);
  if (idx >= kCount) return 0;
  return g_drops[idx].load(std::memory_order_relaxed);
}

}  // namespace queue_metrics
}  // namespace btclock
