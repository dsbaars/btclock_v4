// Pure-logic JSON parser for the Foundry USA Pool v2 earnings response.
//
// Endpoint: GET https://api.foundryusapool.com/v2/earnings/<subacct>
//                ?coin=BTC&startDateUnixMs=<ms>
// Auth:     HTTP header "X-API-KEY: <user's api key>"
// Shape:    array of daily aggregated earnings, newest last (per the
//           OpenAPI spec at api.foundryusapool.com/openapi-spec.json,
//           getEarningsV2). Each entry:
//             {
//               "startTime":         "2024-...T00:00:00.000+00:00",
//               "endTime":           "2024-...T00:00:00.000+00:00",
//               "totalAmount":       0.000123,   // BTC, post-fee
//               "hashrate":          12345678,   // H/s integer
//               "ppsBaseAmount":     0.0,
//               "txFeeRewardAmount": 0.0,
//               "fppsRatePercent":   100.0,
//               "poolFeeAmount":     0.0,
//               "feeRatePercent":    0.0,
//               "grossEarnings":     0.000123    // BTC, pre-fee
//             }
//           We surface `hashrate` and `totalAmount` (post-fee). The
//           fallback hashrate field names ("shareHashrate", "value",
//           "hashRate") stay supported in case Foundry's docs lag the
//           wire format.
//
// We only need the latest entry — that's the headline number Foundry's
// own dashboard displays.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace foundry {

// Pull a numeric "hashrate-like" field out of one series point. Tries
// a few field names because Foundry's docs have lagged renames.
inline bool ExtractHashrate(const cJSON* point, std::string& out) {
  static const char* const kCandidates[] = {"hashrate", "shareHashrate",
                                            "value", "hashRate"};
  for (const char* name : kCandidates) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(point, name);
    if (cJSON_IsNumber(v)) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%.0f", v->valuedouble);
      out = buf;
      return true;
    }
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
      out = v->valuestring;
      return true;
    }
  }
  return false;
}

inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  // Foundry returns either a bare array or an object wrapping one
  // under "data" / "result". Handle both — cheaper than a head-only
  // peek and matches what real responses have shown.
  cJSON* arr = nullptr;
  if (cJSON_IsArray(root)) {
    arr = root;
  } else if (cJSON_IsObject(root)) {
    arr = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(arr)) {
      arr = cJSON_GetObjectItemCaseSensitive(root, "result");
    }
  }
  if (!cJSON_IsArray(arr)) {
    cJSON_Delete(root);
    // Empty-but-valid response — keep last snapshot.
    return true;
  }

  // Walk to the last element. cJSON's public API is single-linked so
  // an O(N) walk is the cheap-enough portable path.
  cJSON* last = nullptr;
  for (cJSON* p = arr->child; p != nullptr; p = p->next) last = p;
  if (last == nullptr) {
    cJSON_Delete(root);
    return true;
  }

  std::string hashrate;
  if (!ExtractHashrate(last, hashrate)) {
    cJSON_Delete(root);
    return false;
  }
  out.hashrate = std::move(hashrate);

  // totalAmount is the user's net BTC payout for the entry's day
  // (post-fee). Convert to sats with rounding to match Braiins and
  // Ocean (both surface today_reward / payout in sats, post-fee).
  cJSON* total = cJSON_GetObjectItemCaseSensitive(last, "totalAmount");
  if (cJSON_IsNumber(total)) {
    const double btc = total->valuedouble;
    const int64_t sats =
        static_cast<int64_t>(btc * 1e8 + (btc >= 0 ? 0.5 : -0.5));
    out.has_daily_sats = true;
    out.daily_sats = sats;
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace foundry
}  // namespace mining_pools
}  // namespace btclock
