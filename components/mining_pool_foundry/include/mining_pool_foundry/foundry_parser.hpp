// Pure-logic JSON parser for the Foundry USA Pool subaccount-hashrate
// response.
//
// Endpoint: GET https://api.foundryusapool.com/subaccount_hashrate_day/
//                <subaccount>?coin=BTC&startDateUnixMs=<ms>
// Auth:     HTTP header "X-API-KEY: <user's api key>"
// Shape:    a daily hashrate series — array of points, newest last.
//             [{"timestamp": ..., "hashrate": <num H/s>}, ...]
//           Foundry's exact field names are stable across coins; we
//           defensively look up several common spellings ("hashrate",
//           "value", "shareHashrate") so a minor upstream rename doesn't
//           silently zero the screen.
//
// We only need the latest point — that's the headline number Foundry's
// own dashboard displays. The rest of the series would be useful for a
// sparkline screen later, but for now we report only the final value.

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
// a few field names because Foundry's docs are gated and field names
// have shifted between API versions.
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

  // Walk to the last element. cJSON exposes `prev` so we can hop to
  // the tail in O(1) via `arr->child->prev` (it's a circular sibling
  // list internally), but the public-API-safe walk is cheap enough.
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
  cJSON_Delete(root);
  return true;
}

}  // namespace foundry
}  // namespace mining_pools
}  // namespace btclock
