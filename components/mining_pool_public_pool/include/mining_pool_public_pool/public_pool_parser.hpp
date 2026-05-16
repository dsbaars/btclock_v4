// Pure-logic JSON parsers for the public-pool family (public-pool.io,
// gobrrr, blitzpool, local forks).
//
// Per-user shape (parse, from /api/client/<addr>):
//   { "workers": [ { "hashRate": <number>, ... }, ... ], ... }
// Pool-wide shape (parse_pool_global, from /api/pool):
//   { "totalHashRate": <number>, "totalMiners": <int>, ... }
//
// We sum per-worker hashRate for the per-user endpoint; the pool-wide
// endpoint already aggregates. Both paths populate the same
// ParsedStats shape (hashrate + workers) so the dispatch is purely on
// the URL choice in api_url().

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

// Parser for /api/pool — used when the `poolGlobalStats` setting is
// on. Body shape:
//   { "totalHashRate": <number>, "totalMiners": <int>, "blockHeight":
//     <int>, "blocksFound": [...], "fee": <number>, "_cachedAt": <iso> }
// Projects totalHashRate -> ParsedStats.hashrate (decimal string,
// integer h/s) and totalMiners -> ParsedStats.workers so the pool
// screen renders pool-wide figures with the same layout as the
// per-user view.
inline bool parse_pool_global(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  cJSON* hr = cJSON_GetObjectItemCaseSensitive(root, "totalHashRate");
  if (cJSON_IsNumber(hr) && hr->valuedouble > 0.0) {
    out.hashrate =
        std::to_string(static_cast<uint64_t>(std::llround(hr->valuedouble)));
  }
  cJSON* m = cJSON_GetObjectItemCaseSensitive(root, "totalMiners");
  if (cJSON_IsNumber(m)) {
    out.has_workers = true;
    out.workers = static_cast<int32_t>(m->valueint);
  }
  cJSON_Delete(root);
  return true;
}

}  // namespace public_pool
}  // namespace mining_pools
}  // namespace btclock
