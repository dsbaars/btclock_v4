// Pure-logic JSON parser for the ViaBTC pool response.
//
// Endpoint: https://www.viabtc.com/res/openapi/v1/hashrate?coin=BTC
// Auth:     HTTP header "X-API-KEY: <user's api key>"
// Shape:    { "code": 0,
//             "data": { "active_workers": int,
//                       "coin": "BTC",
//                       "hashrate_10min":  "1234567890",
//                       "hashrate_1hour":  "1234567890",
//                       "hashrate_24hour": "1234567890",
//                       "unactive_workers": int },
//             "message": "..." }
//
// Hashrates come back as decimal strings already in H/s, so we pass
// them straight through to ParsedStats — no SI-suffix expansion needed
// (cf. ocean_parser, which sees the same shape from api.ocean.xyz).
//
// Mirrors the 1-hour window the v3 firmware used for ViaBTC's "current"
// hashrate display so the on-device read tracks user-visible web stats.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace viabtc {

inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  // ViaBTC envelopes errors as `code != 0`; bail without writing any
  // fields so the base class keeps the previous snapshot.
  cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
  if (cJSON_IsNumber(code) && code->valuedouble != 0.0) {
    cJSON_Delete(root);
    return true;
  }

  cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (data == nullptr || cJSON_IsNull(data)) {
    cJSON_Delete(root);
    return true;
  }

  // Prefer the 1-hour window — the 10-min reading bounces around with
  // share variance, the 24-hour reading lags new hashrate ramps, and
  // 1-hour is what the ViaBTC web dashboard headlines.
  cJSON* hr = cJSON_GetObjectItemCaseSensitive(data, "hashrate_1hour");
  if (cJSON_IsString(hr) && hr->valuestring != nullptr) {
    out.hashrate = hr->valuestring;
  } else if (cJSON_IsNumber(hr)) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f", hr->valuedouble);
    out.hashrate = buf;
  } else {
    cJSON_Delete(root);
    return false;
  }

  cJSON* w = cJSON_GetObjectItemCaseSensitive(data, "active_workers");
  if (cJSON_IsNumber(w)) {
    out.has_workers = true;
    out.workers = static_cast<int32_t>(w->valuedouble);
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace viabtc
}  // namespace mining_pools
}  // namespace btclock
