// Mining-pool selector — the glue between NVS settings and the
// DataSource registry.
//
// The pool components under components/mining_pool_* each expose a
// concrete DataSource that polls one HTTPS endpoint. The old firmware
// drove a single active pool from the `miningPoolName` setting; v4
// preserves that "one pool at a time" model: only the user-selected
// pool's task runs, so the shared DataSnapshot::pool field never races
// between two producers.
//
// Two NVS namespaces are involved and this helper bridges them:
//
//   "settings" — written by the WebUI via /api/settings. This is the
//                source of truth for `miningPoolName`, `miningPoolStats`
//                (enable gate), `miningPoolUser`, `poolGlobalStats`,
//                `localPoolHost`.
//
//   "pool"     — read by the PoolDataSource subclasses at each poll.
//                Keys are short: `user`, `global`, `local_host`.
//
// The helper mirrors the relevant settings keys into the pool namespace
// at boot (one-way, read-only afterwards). Live PATCHes to /api/settings
// won't take effect until reboot — matches the old firmware, where
// setupDataSource() also only ran at boot.
//
// Pool name strings match PoolFactory::MINING_POOL_NAME_* in the old
// firmware so an in-place upgrade sees no key changes.
//
// List of built-in pools (used to populate the WebUI's availablePools
// dropdown via ControlServer::Config::available_pools).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "data_core/source.hpp"

namespace btclock {
namespace mining_pools {

// Full catalogue — stable order preserved across restarts so the
// WebUI renders the dropdown deterministically.
std::vector<std::string> AvailablePoolNames();

// Returns the built pool DataSource, or nullptr when mining-pool stats
// are disabled or the selection is unknown. Mirrors `settings/*` keys
// into the `pool` namespace as a side-effect (see file-level comment).
std::unique_ptr<DataSource> MakeActivePoolSource();

// True iff the named pool reports a usable per-user daily-earnings value
// (i.e. calling `PoolDataSource::SupportsDailyEarnings()` on a freshly
// built instance would return true). The mining-pool-earnings screen
// (api_id 71) is hidden from /api/settings and skipped by the rotation
// whenever this is false — solo pools and public-pool-family endpoints
// have no payout stream to aggregate, so the slot would forever read
// "0 SATS".
//
// Unknown pool names return the conservative `true` so a future pool
// without an overriding hook is not accidentally demoted to hidden.
bool PoolSupportsDailyEarnings(const std::string& pool_name);

// True iff the named pool produces a forward-looking payout estimate
// (`PoolDataSource::SupportsEstimatedEarnings()`). Gates the Estimated
// Earnings screen (api_id 72) — hidden from /api/settings and skipped
// in rotation otherwise. Defaults to `false` for unknown pools so the
// dedicated estimate slot stays off unless a pool explicitly opts in
// (Blitzpool today).
bool PoolSupportsEstimatedEarnings(const std::string& pool_name);

}  // namespace mining_pools
}  // namespace btclock
