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
  std::optional<int32_t> block_fee;         // rounded sats/vB
  std::optional<double> block_fee_precise;  // sats/vB with decimals

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

  // --- Bitaxe miner stats ---
  // Populated by the components/bitaxe/ local-network HTTP poller
  // against `http://<hostname>/api/system/info`. Only relevant on the
  // two Bitaxe screens; renderers test `.hostname.empty()` or the
  // individual optionals to decide "OFFLINE" vs real values.
  struct BitaxeStats {
    // Echoed from the `bitaxeHostname` NVS pref so renderer labels can
    // show which device the stats belong to. Empty = disabled / not
    // configured.
    std::string hostname;
    // Current hashrate reported by AxeOS in GH/s (float). Hub never
    // rescales; the renderer + panel_texts builder decide whether to
    // collapse to "123GH" / "1.2TH" for display.
    std::optional<double> hashrate_ghs;
    // Best share difficulty as AxeOS reports it — historically a
    // suffixed human string like "15.6M"; newer AxeOS returns a raw
    // number, which the parser canonicalises into the same string
    // form so the snapshot is homogeneous.
    std::optional<std::string> best_diff;
    // Optional supplementary fields — not currently painted on-screen
    // but exposed for future screens / /api/status consumers without a
    // second schema bump.
    std::optional<double> temperature_c;
    std::optional<int32_t> shares_accepted;
  };
  BitaxeStats bitaxe{};

  // Most-recent NIP-57 zap receipt. Populated by the zap-listener
  // callback in main.cpp. Only the newest event is kept — older zaps
  // don't queue because the notification screen is a transient
  // single-shot overlay (see ScreenManager::SetZapNotify). Merge rules:
  // the entry with the larger `received_ms` wins, so a partial snapshot
  // with a stale zap can't overwrite a fresher one.
  struct LatestZap {
    std::optional<int64_t> amount_sats;  // msat / 1000, rounded
    std::string message;                 // zapper's content, trimmed
    int64_t received_ms = 0;             // monotonic ms for timeout
  };
  LatestZap latest_zap{};

  // Merge non-empty fields of `other` into `this`. Returns true iff any
  // field actually changed — callers use that to suppress spurious
  // update notifications.
  bool Merge(const DataSnapshot& other);

  // Convenience — returns nullptr if the currency isn't set yet.
  const std::string* PriceOf(const std::string& ccy) const;
};

}  // namespace btclock
