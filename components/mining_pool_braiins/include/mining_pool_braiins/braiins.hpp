// Braiins Pool HTTPS poller.
//
// Polls pool.braiins.com every minute, parses the per-account BTC
// endpoint, and reports hashrate + today_reward into DataSnapshot.pool.
//
// Auth: the user's Braiins API token is read from NVS (namespace
// "pool", key "user" — migrated from the old firmware's
// `miningPoolUser` key by the prefs upgrade shim). Sent in the
// "Pool-Auth-Token" HTTP header.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class BraiinsPool : public PoolDataSource {
 public:
  BraiinsPool() = default;

  const char* name() const override { return "pool.braiins"; }

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  std::string auth_token() const override;
  const char* pool_name() const override { return "braiins"; }
  const char* logo_filename() const override { return "braiins.bin"; }
  int logo_width() const override { return 37; }
  int logo_height() const override { return 230; }
};

}  // namespace mining_pools
}  // namespace btclock
