// Pure-logic JSON parser for Ocean pool (api.ocean.xyz).
//
// Endpoint: https://api.ocean.xyz/v1/statsnap/<poolUser>
// Shape:    { "result": { "hashrate_300s": <integer | string>,
//                         "estimated_earn_next_block": 0.000123, ... } }
//
// Ocean already returns the hashrate as an integer-valued decimal in
// h/s (no SI unit suffix), so we pass it through unchanged. The
// earn-next-block figure is BTC -> multiply by 1e8 to get sats, which
// mirrors the old firmware's "sats/block" daily-earnings label.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace ocean {

inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;
  cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
  if (result == nullptr || cJSON_IsNull(result)) {
    // Empty result = no sample. Base class keeps the previous snapshot.
    cJSON_Delete(root);
    return true;
  }

  cJSON* hr = cJSON_GetObjectItemCaseSensitive(result, "hashrate_300s");
  if (cJSON_IsString(hr) && hr->valuestring != nullptr) {
    out.hashrate = hr->valuestring;
  } else if (cJSON_IsNumber(hr)) {
    // Some ocean responses have seen the field as a number; handle both.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f", hr->valuedouble);
    out.hashrate = buf;
  } else {
    cJSON_Delete(root);
    return false;
  }

  cJSON* earn = cJSON_GetObjectItemCaseSensitive(
      result, "estimated_earn_next_block");
  if (cJSON_IsNumber(earn)) {
    out.has_daily_sats = true;
    out.daily_sats =
        static_cast<int64_t>(std::llround(earn->valuedouble * 1e8));
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace ocean
}  // namespace mining_pools
}  // namespace btclock
