// Noderunners pool HTTPS poller.
//
// Polls pool.noderunners.network every minute, parses the ckpool-
// family per-user endpoint, and reports hashrate into
// DataSnapshot.pool. No daily-earnings, no workers.
//
// User read from NVS pool/user. If the pref `pool/global` is true,
// the global-stats URL is used instead (same shape, aggregates across
// all users).

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class NoderunnersPool : public PoolDataSource {
 public:
  NoderunnersPool() = default;

  const char* name() const override { return "pool.noderunners"; }

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "noderunners"; }
  // Solo pool — raw hashrate only, no payout stream to report.
  bool SupportsDailyEarnings() const override { return false; }
};

}  // namespace mining_pools
}  // namespace btclock
