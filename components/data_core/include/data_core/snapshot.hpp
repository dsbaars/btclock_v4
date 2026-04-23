// Shared data snapshot — the type screens read from.
//
// Every field the firmware can show on a screen lives here. Sources
// populate only the fields/keys they cover; the DataHub merges partial
// snapshots into a single live copy and notifies the app on change.
// Screens never touch a source directly — they operate on this struct
// alone. Swapping btclock WS v2 for mempool.space + Kraken, Nostr, or
// any mix is therefore a main.cpp wiring change, nothing else.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace btclock {

struct DataSnapshot {
  // --- Blocks ---
  std::optional<uint32_t> block_height;
  std::optional<int32_t>  block_fee;          // rounded sats/vB
  std::optional<double>   block_fee_precise;  // sats/vB with decimals

  // --- Prices ---
  // Currency code ("USD","EUR","GBP","JPY","AUD","CAD",…) → formatted
  // price string. Sources that publish strings (server-side precision)
  // store them verbatim; sources that publish numbers format to a string
  // here so the map is homogeneous. Renderers parse as needed.
  std::map<std::string, std::string> prices;

  // --- Mining pool stats ---
  // Populated by per-pool HTTPS pollers under
  // components/mining_pool/*. Only the active pool source writes here;
  // the screen renderer reads the whole struct via PoolStats().
  // Field meanings mirror src/lib/data_sources/mining_pool/pool_stats.hpp
  // from the old firmware, widened for the homogeneous snapshot shape.
  struct PoolStats {
    // Pool identity (e.g. "braiins", "ocean"). Stable across polls;
    // lets the renderer pick the right logo / label.
    std::string name;
    // Human-readable hashrate string as the pool reports it
    // ("123000000000000"-style integer, no unit suffix — formatter
    // rescales for display). Empty string means "no sample yet".
    std::string hashrate;
    // Today's earnings in sats (whole-sats, no decimals). nullopt when
    // the pool's API does not publish a per-day number.
    std::optional<int64_t> daily_sats;
    // Worker count, if the pool publishes one. nullopt otherwise.
    std::optional<int32_t> workers;
  };
  PoolStats pool{};

  // Reserved for sources not yet implemented — Bitaxe stats will add
  // fields here once that source lands. Adding new fields is an ABI
  // addition only, safe for all existing screens.

  // Merge non-empty fields of `other` into `this`. Returns true iff any
  // field actually changed — callers use that to suppress spurious
  // update notifications.
  bool Merge(const DataSnapshot& other);

  // Convenience — returns nullptr if the currency isn't set yet.
  const std::string* PriceOf(const std::string& ccy) const;
};

}  // namespace btclock
