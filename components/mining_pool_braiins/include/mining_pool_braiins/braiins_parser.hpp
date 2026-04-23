// Pure-logic JSON parser for the Braiins pool response.
//
// Endpoint: https://pool.braiins.com/accounts/profile/json/btc/
// Auth:     HTTP header "Pool-Auth-Token: <user's api token>"
// Shape:    { "btc": { "hash_rate_5m": 1234.5,
//                      "hash_rate_unit": "Th/s",
//                      "today_reward": 0.000123, ... } }
//
// Factored into a parser-only header so the host-only test can compile
// without ESP-IDF. The implementation only depends on cJSON + stdlib.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace braiins {

// Old firmware's Braiins-specific unit table (not the generic single-
// character one — Braiins sends "Th/s", "Eh/s", etc. as a string).
inline int unit_exponent(const char* unit) {
  if (unit == nullptr) return -1;
  if (std::strcmp(unit, "Zh/s") == 0) return 21;
  if (std::strcmp(unit, "Eh/s") == 0) return 18;
  if (std::strcmp(unit, "Ph/s") == 0) return 15;
  if (std::strcmp(unit, "Th/s") == 0) return 12;
  if (std::strcmp(unit, "Gh/s") == 0) return 9;
  if (std::strcmp(unit, "Mh/s") == 0) return 6;
  if (std::strcmp(unit, "Kh/s") == 0) return 3;
  return -1;
}

// Parse a Braiins JSON body into `out`. Returns true on success.
// `out.name` is left untouched — the caller pre-sets it.
inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;
  cJSON* btc = cJSON_GetObjectItemCaseSensitive(root, "btc");
  if (btc == nullptr || cJSON_IsNull(btc)) {
    cJSON_Delete(root);
    // Old firmware returned hashrate="0" in this case; we treat it as
    // "no sample" and leave hashrate empty so the base class skips the
    // report and keeps the previous snapshot.
    return true;
  }

  cJSON* unit = cJSON_GetObjectItemCaseSensitive(btc, "hash_rate_unit");
  cJSON* rate5m = cJSON_GetObjectItemCaseSensitive(btc, "hash_rate_5m");
  if (!cJSON_IsString(unit) || !cJSON_IsNumber(rate5m)) {
    cJSON_Delete(root);
    return false;
  }
  const int exp = unit_exponent(unit->valuestring);
  if (exp < 0) {
    cJSON_Delete(root);
    return false;
  }

  // Match old firmware formatting exactly: rounded integer, then
  // (exp) trailing zeros. This keeps downstream parseHashrateString
  // behaviour identical to the old renderer.
  const long long rounded =
      static_cast<long long>(std::llround(rate5m->valuedouble));
  std::string hashrate = std::to_string(rounded);
  hashrate.append(static_cast<size_t>(exp), '0');
  out.hashrate = std::move(hashrate);

  cJSON* reward = cJSON_GetObjectItemCaseSensitive(btc, "today_reward");
  if (cJSON_IsNumber(reward)) {
    out.has_daily_sats = true;
    out.daily_sats =
        static_cast<int64_t>(std::llround(reward->valuedouble * 1e8));
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace braiins
}  // namespace mining_pools
}  // namespace btclock
