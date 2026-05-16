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

  cJSON* sats = cJSON_GetObjectItemCaseSensitive(root, "balanceSats");
  if (cJSON_IsNumber(sats)) {
    // balanceSats can be non-integer if the pool ever switches to
    // fractional-sat accounting; round to the nearest sat. Zero
    // leaves has_daily_sats=false so the earnings screen renders
    // "n/a" rather than "0 SATS" for solo addresses.
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
