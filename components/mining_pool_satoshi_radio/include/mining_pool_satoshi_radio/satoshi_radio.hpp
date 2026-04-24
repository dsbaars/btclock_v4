// Satoshi Radio pool HTTPS poller.
//
// Shares the ckpool-family response shape with Noderunners and ckpool
// (top-level {"hashrate1m": "<value><unit>"}), but points at
// pool.satoshiradio.nl. Parser is the shared ckpool_family walker.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class SatoshiRadioPool : public PoolDataSource {
 public:
  SatoshiRadioPool() = default;

  const char* name() const override { return "pool.satoshiradio"; }

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "satoshiradio"; }
  // Solo pool — ckpool-family per-user JSON exposes only hashrate1m.
  bool SupportsDailyEarnings() const override { return false; }
};

}  // namespace mining_pools
}  // namespace btclock
