// Foundry USA Pool HTTPS poller.
//
// Polls api.foundryusapool.com every minute, parses the daily-hashrate
// series for the configured subaccount, and reports the latest sample
// into DataSnapshot.pool. Auth: X-API-KEY + a per-account subaccount
// name path segment. Both come from the shared mining-pool slots —
// API key in `miningPoolUser` (suppressed via user_is_secret()), the
// subaccount in `miningPoolWorker`. See docs/WEBUI_MINING_POOL_FIELDS.md
// for the per-pool field contract.
//
// Hashrate-only for now. Foundry exposes a separate earnings/payout
// endpoint we deliberately don't poll — the rotation already runs one
// HTTPS request per pool per minute and adding a second would double
// the handshake cost while only refreshing a screen the user may have
// hidden. Hashrate stays the headline figure; an earnings follow-up
// can alternate cycles if a user asks for it.

#pragma once

#include <string>

#include "mining_pool_common/keyed_get_pool_base.hpp"

namespace btclock {
namespace mining_pools {

class FoundryPool : public KeyedGetPoolBase {
 public:
  FoundryPool() = default;

  const char* name() const override { return "pool.foundry"; }

  // No payout stream polled — earnings rotation slot stays hidden when
  // Foundry is the active pool. Revisit if/when we add the earnings
  // endpoint as an alternate-cycle fetch.
  bool SupportsDailyEarnings() const override { return false; }

  bool user_is_secret() const override { return true; }

 protected:
  std::string api_url() const override;
  std::string api_key() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "foundry_usa"; }
};

}  // namespace mining_pools
}  // namespace btclock
