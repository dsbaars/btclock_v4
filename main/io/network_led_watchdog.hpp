// Runtime LED indicator for network faults — distinguishes WiFi
// disconnect from per-source stalls (block-source vs price-source) so
// the user can tell at a glance whether to power-cycle the router or
// just wait for the upstream data to come back.
//
// This watchdog is only ticked by the event loop AFTER the boot tail has
// finished — i.e. once STA has connected AND the first data has landed
// (the gate is `ctx.buttons`, which FinishBoot creates). So it never fires
// during the non-blocking-boot "connecting" or "waiting for first data"
// windows; those show the boot spinner + kWifiConnecting LED, and the
// concurrent-provisioning fallback owns the LED via kSetProvisioning. The
// OutageWatchdog handles the multi-minute brute-force reboot path
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
// The two red-breath tiers (kWifi / kMulti) are DEBOUNCED: they only
// paint after the fault has held for kRedBreathGraceMs. A brief WiFi
// blip self-heals inside the STA auto-reconnect window (~2 s retry +
// association/DHCP) and a momentary double-feed stall is rarely real, so
// firing the alarming red breath for either is just noise — matching the
// v3 firmware, which didn't flash on a transient drop. The quieter
// per-source tiers (kBlock / kPrice) still fire immediately; a single
// stalled feed is low-urgency and seldom transient. A kWifi<->kMulti
// hand-off counts as one continuous red episode and does NOT restart the
// grace window. See ShouldPostIndicator below for the pure decision.
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

// Debounce window for the red-breath tiers (kWifi / kMulti). Sized to
// clear a clean STA auto-reconnect (2 s retry delay + association/DHCP)
// without making a genuine outage feel laggy — the first red breath then
// lands ~grace + one tick after the drop.
inline constexpr uint32_t kRedBreathGraceMs = 5'000;

// Pure-logic helper: decide whether the indicator effect should be
// (re)posted on this tick. Header-inline so host tests link it without
// pulling in network_led_watchdog.cpp (which depends on the LED
// controller + ESP-IDF), mirroring ShouldOutageReboot in wifi_guard.hpp.
//
//   is_red        — the active tier is a red-breath tier (kWifi / kMulti)
//   tier_changed  — this tick crossed into a new tier
//   red_since_ms  — when the current red-breath episode began (a
//                   kWifi<->kMulti hand-off does NOT reseed it)
//   last_post_ms  — when the indicator was last posted
//
// Non-red tiers keep the original behaviour: post on the tier change,
// then re-post on cadence. Red tiers stay silent until they have held
// for grace_ms, then fire once and re-post on cadence.
inline bool ShouldPostIndicator(bool is_red, bool tier_changed, uint32_t now_ms,
                                uint32_t red_since_ms, uint32_t last_post_ms,
                                uint32_t grace_ms, uint32_t cadence_ms) {
  if (!is_red) {
    if (tier_changed) return true;
    return (now_ms - last_post_ms) >= cadence_ms;
  }
  if ((now_ms - red_since_ms) < grace_ms) return false;
  // Grace cleared. Fire the first breath if we have not posted since this
  // episode began (the last post predates red_since_ms), else re-post on
  // cadence. The signed diff is wrap-safe for any window < ~24 days.
  const bool posted_this_episode =
      static_cast<int32_t>(last_post_ms - red_since_ms) >= 0;
  if (!posted_this_episode) return true;
  return (now_ms - last_post_ms) >= cadence_ms;
}

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
  static bool IsRedBreath(Tier t) {
    return t == Tier::kWifi || t == Tier::kMulti;
  }

  Wifi* wifi_ = nullptr;
  std::function<bool()> price_connected_;
  std::function<bool()> blocks_connected_;
  Tier current_tier_ = Tier::kNone;
  uint32_t last_post_ms_ = 0;
  // Start of the current continuous red-breath episode (kWifi/kMulti),
  // for the grace debounce. Seeded on the non-red→red leading edge only.
  uint32_t red_fault_since_ms_ = 0;
};

}  // namespace btclock
