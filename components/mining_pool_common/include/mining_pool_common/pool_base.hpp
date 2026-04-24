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

  // HTTP header to send as "Pool-Auth-Token" (Braiins only). Empty
  // string = no header.
  virtual std::string auth_token() const { return ""; }

  // Stable identifier for DataSnapshot.pool.name and log tags.
  // e.g. "braiins", "ocean".
  virtual const char* pool_name() const = 0;

  // Poll cadence. Default 60 s matches the old firmware's minute timer.
  virtual uint32_t poll_interval_ms() const { return 60 * 1000; }

  // Cap on response body size. Pools with large payloads (public_pool's
  // worker list) raise this. Default 32 KB.
  virtual size_t max_response_bytes() const { return 32 * 1024; }

 public:
  // Whether this pool reports a usable per-user daily sats value. Solo
  // pools (CKPool, Noderunners, Satoshi Radio, Public Pool) only publish
  // raw hashrate — there's no payout stream to aggregate — so the
  // kMiningPoolEarnings screen would forever read "0 SATS". Plugins for
  // those override this to `false`; the /api/settings builder, screen
  // rotation, and POST /api/show/screen?s=71 all gate on this so the
  // earnings slot stays off when a solo pool is active.
  //
  // Default `true` keeps parity with the old firmware for every pool
  // that actually exposes daily_sats (Ocean, Braiins, GoBrrr Pool).
  virtual bool SupportsDailyEarnings() const { return true; }

 private:
  static void TaskTrampoline(void* arg);
  void Run();
  void PollOnce();

  DataHub* hub_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_{false};
};

}  // namespace mining_pools
}  // namespace btclock
