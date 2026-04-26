// ViaBTC Pool HTTPS poller.
//
// Polls www.viabtc.com every minute, parses the public OpenAPI hashrate
// endpoint, and reports hashrate + active worker count into
// DataSnapshot.pool. Auth is a single X-API-KEY header read from the
// shared `miningPoolUser` slot — `user_is_secret()` returns true so the
// /api/settings GET emitter suppresses the raw value. See
// docs/WEBUI_MINING_POOL_FIELDS.md for the per-pool field contract.
//
// ViaBTC publishes only per-account hashrate; there is no per-call
// daily-earnings stream like Ocean's estimated_earn_next_block, so the
// earnings screen is hidden when this pool is selected.

#pragma once

#include <string>

#include "mining_pool_common/keyed_get_pool_base.hpp"

namespace btclock {
namespace mining_pools {

class ViaBtcPool : public KeyedGetPoolBase {
 public:
  ViaBtcPool() = default;

  const char* name() const override { return "pool.viabtc"; }

  // No payout stream available — keeps the earnings rotation slot off
  // when ViaBTC is the active pool.
  bool SupportsDailyEarnings() const override { return false; }

  bool user_is_secret() const override { return true; }

 protected:
  std::string api_url() const override;
  std::string api_key() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "viabtc"; }
};

}  // namespace mining_pools
}  // namespace btclock
