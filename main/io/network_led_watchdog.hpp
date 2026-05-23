// Runtime LED indicator for network faults — distinguishes WiFi
// disconnect from per-source stalls (block-source vs price-source) so
// the user can tell at a glance whether to power-cycle the router or
// just wait for the upstream data to come back.
//
// Boot-time WiFi association is owned by wifi_guard::WaitForConnected;
// this class only watches the post-boot steady-state. The OutageWatchdog
// continues to handle the multi-minute brute-force reboot path
// independently — they cooperate but don't share state.
//
// Tier precedence (most severe wins):
//   kWifi   — STA not connected → continuous slow red breath
//             (kDataError), re-posted every 5 s
//   kMulti  — STA connected but BOTH data sources stale → same red
//             breath (kDataError), re-posted every 5 s
//   kBlock  — only the block source stale → quick purple blink pair
//             (kDataBlockError), re-posted every 10 s
//   kPrice  — only the price source stale → quick amber blink pair
//             (kDataPriceError), re-posted every 10 s
//   kNone   — all good → no effect; the strip's resting paint stands
//
// The re-post cadence is deliberately slow enough that a user-set
// /api/lights/color paint stays visible between indicator pulses
// (the one-shot effects each end with PaintResting that restores the
// resting mirror).

#pragma once

#include <cstdint>
#include <functional>

namespace btclock {

class Wifi;

class NetworkLedWatchdog {
 public:
  NetworkLedWatchdog() = default;

  // Wire post-construction so the watchdog isn't tied to any single
  // owner's lifetime — InitNetwork constructs the watchdog before
  // data sources exist; sources.cpp / init_control_api.cpp install
  // the probes once they're up.
  //
  // Probes are LIFECYCLE-AWARE: they return true ("ok") both when the
  // source is connected AND when the source is intentionally stopped
  // (between Stop() and the next Start()). This is deliberately
  // different from the same data source's IsConnected() probe wired
  // into /api/status, which reports the WIRE truth. Coupling the LED
  // watchdog to lifecycle instead of wire state means every
  // by-design stop — OTA quiesce, /api/stop_datasources, factory
  // reset, dataSource toggle — naturally silences the indicator
  // without per-call-site gating. bd btclock_v4-28n.
  void SetWifi(Wifi* wifi) { wifi_ = wifi; }
  void SetPriceConnected(std::function<bool()> f) {
    price_connected_ = std::move(f);
  }
  void SetBlocksConnected(std::function<bool()> f) {
    blocks_connected_ = std::move(f);
  }

  // Advance the state machine. Call once per second from the event
  // loop. `now_ms` should be a monotonic millisecond clock
  // (esp_timer_get_time()/1000 or equivalent). Cheap when no fault is
  // active — only does work on tier transitions or when the cadence
  // timer fires.
  void Tick(uint32_t now_ms);

  // Test seam: expose the current tier so host-test fixtures can
  // assert state-machine progression without coupling to the LED
  // controller (which doesn't link in host builds).
  enum class Tier : uint8_t { kNone, kWifi, kMulti, kBlock, kPrice };
  Tier current_tier_for_test() const { return current_tier_; }

 private:
  Tier ClassifyFault() const;
  void PostEffectFor(Tier t);
  static uint32_t CadenceMs(Tier t);

  Wifi* wifi_ = nullptr;
  std::function<bool()> price_connected_;
  std::function<bool()> blocks_connected_;
  Tier current_tier_ = Tier::kNone;
  uint32_t last_post_ms_ = 0;
};

}  // namespace btclock
