// Plain result struct for pool parsers — no ESP-IDF dependencies so
// host-only tests can include it. Kept separate from pool_base.hpp
// because the latter pulls in FreeRTOS + esp_err.h.

#pragma once

#include <cstdint>
#include <string>

namespace btclock {
namespace mining_pools {

// Concrete pool parsers fill this. The base class copies the fields
// into DataSnapshot::PoolStats before reporting.
struct ParsedStats {
  std::string name;  // pre-set by base to pool_name()
  std::string hashrate;
  bool has_daily_sats = false;
  int64_t daily_sats = 0;
  bool has_workers = false;
  int32_t workers = 0;
};

}  // namespace mining_pools
}  // namespace btclock
