// Foundry USA Pool HTTPS poller.
//
// Polls api.foundryusapool.com every minute via the v2 earnings
// endpoint, which returns a daily series carrying both hashrate and
// post-fee BTC payout per day. We surface the latest entry's hashrate
// and totalAmount into DataSnapshot.pool. Auth: X-API-KEY header + a
// per-account subaccount name path segment. Both come from the shared
// mining-pool slots — API key in `miningPoolUser` (suppressed via
// user_is_secret()), the subaccount in `miningPoolWorker`. See
// docs/WEBUI_MINING_POOL_FIELDS.md for the per-pool field contract.
//
// Single endpoint: /v2/earnings/<subacct> rather than the legacy
// /subaccount_hashrate_day/ — the v2 response includes hashrate so we
// don't pay an extra HTTPS handshake to surface earnings.

#pragma once

#include <string>

#include "mining_pool_common/keyed_get_pool_base.hpp"

namespace btclock {
namespace mining_pools {

class FoundryPool : public KeyedGetPoolBase {
 public:
  FoundryPool() = default;

  const char* name() const override { return "pool.foundry"; }

  bool SupportsDailyEarnings() const override { return true; }

  bool user_is_secret() const override { return true; }

 protected:
  std::string api_url() const override;
  std::string api_key() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "foundry_usa"; }
};

}  // namespace mining_pools
}  // namespace btclock
