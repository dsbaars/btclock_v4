// Exercises the TLS-gate mutex to assert two threads cannot hold it
// simultaneously. The contract (serialising TLS handshakes) reduces to
// "there is exactly one shared std::mutex, and the normal mutex rules
// apply" — so the test is short.

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "doctest.h"
#include "tls_gate/tls_gate.hpp"

TEST_CASE("tls_gate::mutex returns the same instance each call") {
  auto* a = &btclock::tls_gate::mutex();
  auto* b = &btclock::tls_gate::mutex();
  CHECK(a == b);
}

TEST_CASE("tls_gate::mutex serialises two threads") {
  auto& m = btclock::tls_gate::mutex();
  std::atomic<bool> holder_inside{false};
  std::atomic<bool> try_lock_succeeded{false};

  std::thread holder([&]() {
    std::lock_guard<std::mutex> lk(m);
    holder_inside.store(true, std::memory_order_release);
    // Give the challenger thread time to attempt try_lock while we hold.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    holder_inside.store(false, std::memory_order_release);
  });

  // Wait until the holder has the lock.
  while (!holder_inside.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::thread challenger([&]() {
    // try_lock must fail while holder still has it.
    if (m.try_lock()) {
      try_lock_succeeded.store(true, std::memory_order_release);
      m.unlock();
    }
  });

  challenger.join();
  holder.join();

  CHECK_FALSE(try_lock_succeeded.load(std::memory_order_acquire));

  // After holder exits, the mutex must be acquirable again.
  CHECK(m.try_lock());
  m.unlock();
}
