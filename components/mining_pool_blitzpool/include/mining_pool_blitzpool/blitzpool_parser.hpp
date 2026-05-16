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

// Default PPLNS pool fee (%) — published by /api/pplns/fees. Hardcoded
// here so the parser doesn't need a third HTTP call per poll. If the
// pool flips this value, the projected payout drifts by ≤ 2 % until a
// firmware bump.
inline constexpr double kBlitzpoolPplnsFeePercent = 1.5;

inline bool parse_pplns_balance(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  // Settled ledger balance — the actual sats the pool will pay out
  // once the address clears the minPayoutSats threshold. Zero is a
  // valid sample ("we have data, current settled balance is 0
  // sats") and renders as a literal "0 SATS" so a configured PPLNS
  // miner doesn't see blank digits.
  cJSON* sats = cJSON_GetObjectItemCaseSensitive(root, "balanceSats");
  if (cJSON_IsNumber(sats)) {
    out.has_daily_sats = true;
    out.daily_sats = static_cast<int64_t>(std::llround(sats->valuedouble));
  }

  // Projected-payout inputs for the dedicated Estimated Earnings
  // screen. pool_base multiplies `window_percent / 100` by the live
  // block subsidy (BlockRewardSats(snapshot.block_height)) and then
  // by `(1 - fee/100)` to produce the rendered sats — same formula
  // the public WebUI runs on /app/<addr>/payout-pplns, but the
  // subsidy comes from the current tip instead of a hardcoded
  // pre-halving constant.
  cJSON* pct = cJSON_GetObjectItemCaseSensitive(root, "currentWindowPercent");
  if (cJSON_IsNumber(pct) && pct->valuedouble > 0.0) {
    out.has_window_percent = true;
    out.window_percent = pct->valuedouble;
    out.has_fee_percent = true;
    out.fee_percent = kBlitzpoolPplnsFeePercent;
  }
  cJSON_Delete(root);
  return true;
}

}  // namespace blitzpool
}  // namespace mining_pools
}  // namespace btclock
