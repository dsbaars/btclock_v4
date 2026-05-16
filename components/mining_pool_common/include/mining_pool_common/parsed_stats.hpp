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
  // Forward-looking payout *inputs* — pools that can compute a
  // projection (e.g. Blitzpool PPLNS) report the raw share-window
  // percentage and pool fee here. pool_base multiplies these by the
  // current block subsidy (from the live snapshot's block_height) to
  // produce the rendered `estimated_sats` value. Kept as raw inputs
  // rather than a pre-baked number so the subsidy stays correct
  // across the halving without a per-parser hardcode.
  bool has_window_percent = false;
  double window_percent = 0.0;  // 0..100, e.g. 7.4e-5 = 0.0000074 %
  bool has_fee_percent = false;
  double fee_percent = 0.0;  // 0..100, e.g. 1.5 for a 1.5 % pool fee

  // Pre-baked projected payout, when a pool publishes the value
  // directly rather than supplying the share-window inputs. Today no
  // pool takes this path — Blitzpool funnels through window_percent
  // and lets pool_base scale by BlockRewardSats(height). Reserved so
  // a future pool can short-circuit if its API serves a settled
  // estimate.
  bool has_estimated_sats = false;
  int64_t estimated_sats = 0;
};

}  // namespace mining_pools
}  // namespace btclock
