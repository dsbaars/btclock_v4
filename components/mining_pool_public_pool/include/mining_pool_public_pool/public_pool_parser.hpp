// Pure-logic JSON parser for public-pool.io (and gobrrr, which reuses
// the same response shape).
//
// Shape:
//   { "workers": [ { "hashRate": <number>, ... }, ... ], ... }
//
// The pool exposes a per-client endpoint listing the worker objects
// for a given pubkey. We sum hashRate across workers and return the
// total as the `hashrate` field. No daily earnings, no worker-count
// exposed in DataSnapshot yet (the count is available — could be
// added later if a screen needs it).

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "cJSON.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace public_pool {

inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  cJSON* workers = cJSON_GetObjectItemCaseSensitive(root, "workers");
  if (!cJSON_IsArray(workers)) {
    cJSON_Delete(root);
    // Old firmware still reported hashrate=0 in this shape. Treat as
    // no sample so the base class keeps the previous snapshot.
    return true;
  }

  uint64_t total = 0;
  int worker_count = 0;
  cJSON* w = nullptr;
  cJSON_ArrayForEach(w, workers) {
    cJSON* rate = cJSON_GetObjectItemCaseSensitive(w, "hashRate");
    if (cJSON_IsNumber(rate)) {
      total += static_cast<uint64_t>(std::llround(rate->valuedouble));
    }
    ++worker_count;
  }

  if (total == 0) {
    cJSON_Delete(root);
    return true;  // all workers offline — keep last snapshot
  }

  out.hashrate = std::to_string(total);
  out.has_workers = true;
  out.workers = static_cast<int32_t>(worker_count);
  cJSON_Delete(root);
  return true;
}

}  // namespace public_pool
}  // namespace mining_pools
}  // namespace btclock
