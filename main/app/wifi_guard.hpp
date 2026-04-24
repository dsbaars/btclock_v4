// WiFi guard — block the caller until the STA gets an IP, so network-
// dependent subsystems (data sources, NTP, etc.) come up cleanly.
//
// Escalates back to the provisioning portal after sustained terminal
// auth failures (wrong saved password, AP renamed, etc.) so the device
// isn't bricked by bad creds once the USB cable is put away.
//
// Also exposes a long-outage soft watchdog (OutageWatchdog) the main
// loop pumps on each tick: if STA has been continuously disconnected
// for N minutes, call esp_restart(). Ported from the Arduino firmware's
// checkWiFiConnection() 10-min brute-force recovery path — covers the
// "creds are fine, router is flaky" case that the strikes-clear path
// above deliberately leaves alone.

#pragma once

#include <cstdint>

namespace btclock {

class Prefs;
class Wifi;

// Blocks until wifi.state() == kConnected. Logs a "still no IP" line
// every `log_every_ms` milliseconds so the serial console shows progress
// during slow association.
//
// After `max_terminal_strikes` consecutive terminal disconnects (auth
// fail, no AP, etc.) the function clears `net/ssid` in `net_prefs` and
// calls `esp_restart()` — the device comes back up in provisioning
// mode so the user can enter new credentials. Default is generous
// enough (10) to tolerate a long router boot cycle without nuking
// good creds, but short enough to recover within ~90 s on truly wrong
// creds (strikes fire every ~7-9 s).
void WaitForConnected(Wifi& wifi, Prefs& net_prefs,
                      uint32_t log_every_ms = 10'000,
                      uint32_t max_terminal_strikes = 10);

// Pure-logic helper: decide whether to trigger an outage reboot given
// the timestamp of the current outage start, the current time, and the
// configured outage-reboot threshold in minutes.
//
// Header-inline so host tests can link it without pulling in the rest
// of wifi_guard.cpp (which depends on ESP-IDF headers).
//   disconnected_since_ms == 0 → not currently disconnected, return false
//   outage_minutes == 0        → watchdog disabled, return false
//   elapsed >= threshold       → return true
inline bool ShouldOutageReboot(uint32_t disconnected_since_ms, uint32_t now_ms,
                               uint32_t outage_minutes) {
  if (outage_minutes == 0) return false;
  if (disconnected_since_ms == 0) return false;
  const uint32_t threshold_ms = outage_minutes * 60u * 1000u;
  return (now_ms - disconnected_since_ms) >= threshold_ms;
}

// Edge-tracked disconnect monitor. Piggybacks on the caller's existing
// tick (main loop's 1 Hz pump) — no task of its own so we don't spend
// stack + a priority slot on something that fires at most once per
// outage.
class OutageWatchdog {
 public:
  // `outage_reboot_minutes` is loaded from the settings NVS namespace
  // (kWifiRebootMin) by the caller. 0 disables.
  explicit OutageWatchdog(uint32_t outage_reboot_minutes);

  // Advance the watchdog. On the connected→disconnected edge, stamp
  // the outage start; on any connected tick, clear it; on a tick past
  // the configured threshold, log and reboot.
  //
  // `now_ms` should be esp_timer_get_time()/1000 or equivalent
  // monotonic millisecond clock.
  void Tick(Wifi& wifi, uint32_t now_ms);

  // Runtime update hook — settings PATCH of wifiRebootMin can call this
  // so the change takes effect without reboot. If not wired up the
  // value set at construction time stays authoritative.
  void SetOutageRebootMinutes(uint32_t minutes) {
    outage_reboot_minutes_ = minutes;
  }

  uint32_t disconnected_since_ms_for_test() const {
    return disconnected_since_ms_;
  }

 private:
  uint32_t disconnected_since_ms_ = 0;
  uint32_t outage_reboot_minutes_ = 0;
  bool was_connected_ = false;
};

}  // namespace btclock
