// Pure-logic JSON parser for Blitzpool's /api/pplns/<addr> endpoint.
//
// The /api/client/<addr> and /api/pool endpoints both share their
// shape with upstream public-pool, so parse + parse_pool_global live
// in mining_pool_public_pool/public_pool_parser.hpp and are reused
// from there. Only the PPLNS-specific balance parser is here.
//
// Header-only so host tests can include it without dragging the
// ESP-IDF prefs + http plumbing into the host build.

#pragma once

#include <cmath>
#include <cstdint>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace blitzpool {

inline bool parse_pplns_balance(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  // Settled ledger balance only. For PPLNS miners below the payout
  // threshold this is zero — the *projected* payout estimate
  // (currentWindowPercent × next-block reward × (1 - fee)) is
  // intentionally NOT folded into daily_sats: that field carries
  // settled sats on other pools (Braiins, Ocean), and mixing a live
  // estimate that can drop by >50 % on a single share-window update
  // would silently break the earnings screen's semantic on those
  // pools. Projected payout is tracked under btclock_v4 follow-up
  // for a dedicated screen (see commit message).
  cJSON* sats = cJSON_GetObjectItemCaseSensitive(root, "balanceSats");
  if (cJSON_IsNumber(sats)) {
    const double v = sats->valuedouble;
    if (v > 0.0) {
      out.has_daily_sats = true;
      out.daily_sats = static_cast<int64_t>(std::llround(v));
    }
  }
  cJSON_Delete(root);
  return true;
}

}  // namespace blitzpool
}  // namespace mining_pools
}  // namespace btclock
