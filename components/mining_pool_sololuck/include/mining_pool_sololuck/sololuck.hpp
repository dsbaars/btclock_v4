// SoloLuck HTTPS poller (sololuck.io).
//
// SoloLuck is a 0%-fee true-solo pool running a patched ckpool core,
// hosted in Jakarta. The HTTP API is path-compatible with upstream solo
// ckpool — `/users/<address>` returns JSON with the same `hashrate1m`
// SI-suffixed string — so the shared ckpool-family parser handles it
// unchanged.
//
// Unlike ckpool.org and NerdMiner Pool there are no regional mirrors,
// so this needs no `base_url()` virtual and no base class — one host,
// one id.
//
// Solo pool: hashrate only, never a per-user daily payout.
//
// No upstream logo (verified against
// https://git.btclock.dev/btclock/mining-pool-logos as of 2026-09-01 —
// it holds blitzpool, braiins, gobrrr, noderunners and ocean), so the
// pool_base.hpp logo getters keep their defaults and the renderer
// paints the text-split fallback exactly as it does for ckpool and
// nerdminer.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class SoloLuckPool : public PoolDataSource {
 public:
  SoloLuckPool() = default;
  const char* name() const override { return "pool.sololuck"; }

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "sololuck"; }
  bool SupportsDailyEarnings() const override { return false; }
};

}  // namespace mining_pools
}  // namespace btclock
