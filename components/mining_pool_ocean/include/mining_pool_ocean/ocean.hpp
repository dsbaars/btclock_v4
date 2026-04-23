// Ocean Pool HTTPS poller.
//
// Polls api.ocean.xyz every minute, parses the per-user statsnap
// endpoint, reports hashrate + estimated_earn_next_block (as sats)
// into DataSnapshot.pool. Pool user is read from NVS (pool/user).

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class OceanPool : public PoolDataSource {
 public:
  OceanPool() = default;

  const char* name() const override { return "pool.ocean"; }

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "ocean"; }
};

}  // namespace mining_pools
}  // namespace btclock
