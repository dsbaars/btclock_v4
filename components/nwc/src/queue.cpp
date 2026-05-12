#include "nwc/queue.hpp"

#include <chrono>
#include <utility>

namespace btclock {
namespace nwc {

NotificationQueue::NotificationQueue(size_t capacity)
    : capacity_(capacity == 0 ? kDefaultCapacity : capacity) {}

NotificationQueue::~NotificationQueue() {
  Shutdown();
}

bool NotificationQueue::TryPush(RawNotification item) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (shutdown_ || items_.size() >= capacity_) {
      dropped_.fetch_add(1);
      return false;
    }
    items_.push_back(std::move(item));
    pushed_.fetch_add(1);
  }
  cv_.notify_one();
  return true;
}

bool NotificationQueue::WaitPop(RawNotification& out, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lk(mu_);
  if (items_.empty() && !shutdown_) {
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                 [this] { return shutdown_ || !items_.empty(); });
  }
  if (items_.empty()) return false;
  out = std::move(items_.front());
  items_.pop_front();
  popped_.fetch_add(1);
  return true;
}

void NotificationQueue::Shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    shutdown_ = true;
  }
  cv_.notify_all();
}

size_t NotificationQueue::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return items_.size();
}

}  // namespace nwc
}  // namespace btclock
