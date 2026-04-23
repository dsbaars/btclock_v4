// Shared pure-logic parser for the ckpool-family JSON response shape.
//
// Noderunners, Satoshi Radio, and the plain ckpool.org API all return
// JSON with a `hashrate1m` field like "123.4P" — a decimal number plus
// a single-character SI-unit suffix. None of these three pools report
// daily earnings or worker counts, so only the hashrate is populated.
//
// The per-pool adapters differ only in the URL (and Satoshi Radio's
// override of getGlobalStatsUrl). All three share this parser.

#pragma once

#include <string>

#include "cJSON.h"
#include "mining_pool_common/hashrate.hpp"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {
namespace mining_pools {
namespace ckpool_family {

// Parse the response. Returns true even when hashrate1m is absent or
// "0" (in those cases `out.hashrate` stays empty, which signals "no
// sample yet" to the base class). Returns false only on JSON errors
// the caller should retry.
inline bool parse(const char* body, ParsedStats& out) {
  cJSON* root = cJSON_Parse(body);
  if (root == nullptr) return false;

  cJSON* hr = cJSON_GetObjectItemCaseSensitive(root, "hashrate1m");
  if (cJSON_IsString(hr) && hr->valuestring != nullptr) {
    const std::string hr_str = hr->valuestring;
    if (auto normalised = normalise_hashrate(hr_str)) {
      out.hashrate = std::move(*normalised);
    }
    // else: empty hashrate, treated as "no sample" — base class skips
    // the report and keeps the previous snapshot.
  }

  cJSON_Delete(root);
  return true;
}

}  // namespace ckpool_family
}  // namespace mining_pools
}  // namespace btclock
