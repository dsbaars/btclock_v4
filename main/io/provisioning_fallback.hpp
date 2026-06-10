// Concurrent-provisioning fallback: keep STA retrying while the
// provisioning portal comes up alongside it (APSTA) when the saved
// network can't be reached, instead of the old "wipe creds + reboot
// into SoftAP" escalation.
//
// Behaviour (have-creds boot path only):
//   * Boot kicks off a non-blocking STA connect; the boot sequence does
//     NOT block on GOT_IP.
//   * If STA hasn't connected within `grace_ms`, bring up the SoftAP +
//     captive portal CONCURRENTLY (the radio is already APSTA-capable —
//     see Wifi::StartSoftAp). STA keeps retrying forever; creds are never
//     wiped.
//   * The original network coming back -> STA reconnects on its own and
//     the fallback AP is torn down again (back to STA-only).
//   * A *different* network can be configured via the portal while STA
//     is still down — the verify-before-save TryConnect path already
//     works in APSTA.
//
// Mid-run outages after a successful connect are NOT this module's job:
// they belong to OutageWatchdog (reboot after N minutes), which on the
// next boot re-enters this grace -> fallback path if the network is
// still gone. So the fallback only arms before the FIRST successful
// connect since boot — `has_connected_once` latches it off afterwards.
//
// The two decision predicates below are pure so host tests can pin the
// grace/teardown edges without an ESP-IDF dependency (same pattern as
// ShouldOutageReboot in wifi_guard.hpp). The stateful ProvisioningFallback
// coordinator that drives the actual SoftAP/portal/SNTP side effects
// lives in provisioning_fallback.cpp.

#pragma once

#include <cstdint>

namespace btclock {

// Decide whether to bring up the concurrent provisioning AP.
//   has_connected_once — true once STA has had at least one GOT_IP since
//                        boot. Mid-run outages are OutageWatchdog's job,
//                        so we only arm before the first connect.
//   ap_up              — true if the fallback AP/portal is already up.
//   sta_connected      — true while STA currently holds an IP.
//   now_ms, boot_ms    — monotonic ms clock and the stamp captured when
//                        the STA connect was kicked off.
//   grace_ms           — how long to give STA before falling back. 0 ==
//                        bring the AP up immediately (no grace window).
inline bool ShouldStartFallbackAp(bool has_connected_once, bool ap_up,
                                  bool sta_connected, uint32_t now_ms,
                                  uint32_t boot_ms, uint32_t grace_ms) {
  if (has_connected_once) return false;
  if (ap_up) return false;
  if (sta_connected) return false;
  return (now_ms - boot_ms) >= grace_ms;
}

// Decide whether to tear the fallback AP back down. Once STA is back on
// the saved network we no longer need the portal broadcasting, so drop
// it and return to STA-only. (The user accepted that an in-progress
// portal session can be cut short if STA happens to recover mid-config —
// the verify-before-save Save path reboots on its own anyway.)
inline bool ShouldTeardownAp(bool ap_up, bool sta_connected) {
  return ap_up && sta_connected;
}

}  // namespace btclock
