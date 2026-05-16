// Base class for mining-pool HTTP pollers.
//
// Every pool source in components/mining_pool_* polls a JSON endpoint
// on a fixed cadence and reports the parsed stats into DataSnapshot.pool.
// The HTTPS handshake, response buffering, JSON parse retry policy, and
// FreeRTOS task lifecycle are identical across all seven pools — only
// the URL, request headers, and JSON parser change. PoolDataSource owns
// the common plumbing; concrete classes override three pure virtuals.
//
// Threading:
//   Start() spawns a dedicated poll task. Each tick the task:
//     1. Resolves api_url() + auth_token() (may read NVS prefs).
//     2. Takes btclock::tls_gate::mutex() JUST around the GET call —
//        the window in which mbedtls performs the handshake. The lock
//        is released before the hub->Report(). This mirrors the old
//        firmware's HttpHelper::beginScoped() but narrower, so other
//        handshakes can proceed while we parse the response body.
//     3. Passes the response buffer to parse_response(). On success,
//        merges into a partial DataSnapshot and calls hub.Report().
//     4. Sleeps for poll_interval_ms() (default 60 s).
//   Stop() sets a stop flag and joins the task.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mining_pool_common/parsed_stats.hpp"

namespace btclock {

class DataHub;

namespace mining_pools {

class PoolDataSource : public DataSource {
 public:
  PoolDataSource();
  ~PoolDataSource() override;

  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

 protected:
  // Full HTTPS URL to GET. May read NVS prefs for the pool-user path
  // component. Called each poll, so it's fine for the URL to vary with
  // user prefs between polls.
  virtual std::string api_url() const = 0;

  // Parse a response body (NUL-terminated JSON string). Populate `out`
  // with the fields this pool provides. Return true on success, false
  // to indicate the response was unparseable — in which case the
  // previous snapshot value is kept (no overwrite).
  //
  // `out.name` is pre-set to pool_name() so concrete parsers can leave
  // it alone; they only fill hashrate / daily_sats / workers.
  virtual bool parse_response(const char* body, ParsedStats& out) const = 0;

  // HTTP header to send as auth_header_name() (Braiins only by default).
  // Empty string = no header.
  virtual std::string auth_token() const { return ""; }

  // Header name carrying the auth_token() value. Defaults to Braiins's
  // "Pool-Auth-Token"; the keyed_get mix-in (ViaBTC, Foundry) overrides
  // to "X-API-KEY". Kept as a virtual so the header-name choice lives
  // next to the token-source choice in one subclass — avoids a parallel
  // table of {pool -> header} in the base.
  virtual const char* auth_header_name() const { return "Pool-Auth-Token"; }

  // Stable identifier for DataSnapshot.pool.name and log tags.
  // e.g. "braiins", "ocean".
  virtual const char* pool_name() const = 0;

  // Poll cadence. Default reads `settings/poolPollSec` (5..3600 s, default
  // 60 s — matches the old firmware's minute timer) on every Run() tick so
  // a live PATCH lands on the next poll without reboot.
  virtual uint32_t poll_interval_ms() const;

  // Cap on response body size. Pools with large payloads (public_pool's
  // worker list) raise this. Default 32 KB.
  virtual size_t max_response_bytes() const { return 32 * 1024; }

  // Optional second URL polled on the same tick, after the primary
  // fetch + parse succeeds. Used by Blitzpool to layer the PPLNS
  // balance (/api/pplns/<addr>) on top of the worker list
  // (/api/client/<addr>) without spawning a parallel poll task that
  // would step on the hub's all-or-nothing PoolStats merge. Default
  // empty = no secondary call; the base class skips the second fetch
  // entirely so other pools pay zero cost.
  virtual std::string secondary_api_url() const { return ""; }

  // Parser for the secondary body. Called with the same `out` already
  // populated by parse_response(), so subclasses fold fields together
  // (typically: fill out.daily_sats from a balance/earnings field).
  // Return false on parse failure — the base discards the secondary
  // result and reports just the primary parse.
  virtual bool parse_secondary_response(const char* /*body*/,
                                        ParsedStats& /*out*/) const {
    return true;
  }

  // Cap on secondary response body. PPLNS balance is ~100 bytes; keep
  // the default tight so a misconfigured upstream can't eat heap.
  virtual size_t max_secondary_response_bytes() const { return 4 * 1024; }

 public:
  // True when miningPoolUser holds a secret API key rather than a public
  // identifier (address, username). The /api/settings GET emitter
  // suppresses the raw value and emits a miningPoolUserSet bool instead,
  // mirroring the httpAuthPass pattern. Default false matches every
  // existing pool whose user slot is a public payout address / username
  // / worker name.
  virtual bool user_is_secret() const { return false; }

  // Whether this pool reports a usable per-user daily sats value. Solo
  // pools (CKPool, Noderunners, Satoshi Radio, Public Pool) only publish
  // raw hashrate — there's no payout stream to aggregate — so the
  // kMiningPoolEarnings screen would forever read "0 SATS". Plugins for
  // those override this to `false`; the /api/settings builder, screen
  // rotation, and body-first POST /api/show/screen {"s":71} all gate on
  // this so the
  // earnings slot stays off when a solo pool is active.
  //
  // Default `true` keeps parity with the old firmware for every pool
  // that actually exposes daily_sats (Ocean, Braiins).
  virtual bool SupportsDailyEarnings() const { return true; }

  // Whether this pool produces a forward-looking payout estimate
  // (ParsedStats.estimated_sats — e.g. Blitzpool PPLNS' projection
  // from currentWindowPercent × next-block reward). Gates the
  // dedicated Estimated Earnings screen (api_id 72) in the settings
  // catalog and the rotation predicate so pools that only report
  // settled payouts don't advertise the slot.
  //
  // Default `false` — opt-in. Only Blitzpool overrides today.
  virtual bool SupportsEstimatedEarnings() const { return false; }

  // Mining-pool logo metadata. Mirrors v3's
  // `MiningPoolInterface::hasLogo()` / `getLogoFilename()` /
  // `getLogoWidth()` / `getLogoHeight()` quartet so the runtime fetcher
  // (bd btclock_v4-5yi) can ask each pool whether to attempt a download
  // and how to interpret the resulting raw 1-bpp byte stream — the
  // upstream `mining-pool-logos` repo ships header-less files (just
  // `width*height/8` bytes), so the renderer needs the dimensions from
  // a side channel. Default empty filename = "no logo, paint the
  // text-split fallback"; pools that ship a vendored bitmap also
  // declare the same dimensions in main/screens/assets/pool_logos.cpp.
  virtual const char* logo_filename() const { return ""; }
  virtual int logo_width() const { return 0; }
  virtual int logo_height() const { return 0; }
  bool has_logo() const {
    const char* f = logo_filename();
    return f != nullptr && f[0] != '\0';
  }

 private:
  static void TaskTrampoline(void* arg);
  void Run();
  void PollOnce();

  DataHub* hub_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_{false};
  // Given by Run() right before vTaskDelete(nullptr); waited on by
  // Stop() with a timeout so the OTA pre-flash hook can guarantee the
  // poll task has exited before flash erase begins. Without this, a
  // poll mid-HTTPS handshake could still be alive (and holding mbedtls
  // / TLS scratch) when esp_ota_write disables cache.
  SemaphoreHandle_t done_ = nullptr;
};

}  // namespace mining_pools
}  // namespace btclock
