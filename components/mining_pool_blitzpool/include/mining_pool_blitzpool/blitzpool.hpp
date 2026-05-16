// Blitzpool HTTPS poller.
//
// Blitzpool (https://blitzpool.yourdevice.ch:3334) is a custom fork of
// public-pool that adds Stratum V2 + PPLNS + group-solo modes. Three
// API endpoints feed the display:
//
//   GET /api/client/<addr>  -> workers + per-address hashrate
//                              (same shape as upstream public-pool;
//                               reuses public_pool::parse)
//   GET /api/pplns/<addr>   -> balanceSats (PPLNS / group-solo pending
//                              payout). Polled as the secondary URL
//                              and folded into ParsedStats.daily_sats.
//   GET /api/pool           -> pool-wide totalHashRate + totalMiners
//                              (used when the `poolGlobalStats`
//                              setting is on, parity with Noderunners
//                              / Satoshi Radio).
//
// The /api/pplns/<addr> body is the same for all 3 payout modes; solo
// addresses report balanceSats=0 and the earnings screen falls back
// to "n/a". Phase-3 group-solo work (btclock_v4-ae0) is tracked
// separately.

#pragma once

#include <string>

#include "mining_pool_blitzpool/blitzpool_parser.hpp"
#include "mining_pool_common/parsed_stats.hpp"
#include "mining_pool_public_pool/public_pool.hpp"

namespace btclock {
namespace mining_pools {

class BlitzPool : public PublicPoolBase {
 public:
  BlitzPool() = default;
  const char* name() const override { return "pool.blitzpool"; }

  // Surface the earnings screen so PPLNS / group-solo addresses see
  // their pending balance. Solo addresses report balanceSats=0 and
  // simply leave DataSnapshot.pool.daily_sats nullopt; the earnings
  // screen will show "n/a" in that case (same UX as Ocean before any
  // payouts) and the user can disable the screen via settings.
  bool SupportsDailyEarnings() const override { return true; }

 protected:
  std::string api_url() const override;
  std::string secondary_api_url() const override;
  // Dispatches on the `pool/global` NVS flag between the public_pool
  // worker-list parser (per-address) and parse_pool_global (pool-wide).
  bool parse_response(const char* body, ParsedStats& out) const override;
  const char* pool_name() const override { return "blitzpool"; }
  // 122x122 1-bpp logo from the mining-pool-logos repo (same size as
  // ocean/noderunners/gobrrr — fits the 2.13" panel's 122 px tall slot).
  const char* logo_filename() const override { return "blitzpool.bin"; }
  int logo_width() const override { return 122; }
  int logo_height() const override { return 122; }
};

}  // namespace mining_pools
}  // namespace btclock
