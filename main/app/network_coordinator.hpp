// Runtime driver for the concurrent-provisioning fallback.
//
// Ticked once per event-loop iteration (1 Hz heartbeat + wakeups). It
// delegates the grace / teardown decisions to the pure predicates in
// io/provisioning_fallback.hpp and performs the side effects:
//   * first STA connect  -> wire the internet services (SNTP + data
//                           sources + zap + NWC) that boot deliberately
//                           deferred, since there was no connection yet.
//   * grace elapsed       -> bring up the SoftAP + portal CONCURRENTLY
//                           (StartProvisioningPortal) while STA keeps
//                           retrying the saved network.
//   * STA reconnect       -> tear the portal back down (StopProvisioning-
//                           Portal) and return to STA-only.
//
// Constructed only on the have-creds boot path (init_network.cpp). The
// boot clock is captured on the first Tick so it shares the event loop's
// MsNow() timebase without a separate clock dependency.

#pragma once

#include <cstdint>

namespace btclock {

struct AppCtx;
class Wifi;

class NetworkCoordinator {
 public:
  // grace_ms: how long to give STA to reach the saved network before the
  // fallback portal comes up alongside it. 0 == bring it up immediately.
  explicit NetworkCoordinator(AppCtx& ctx, uint32_t grace_ms = 20'000);

  void Tick(Wifi& wifi, uint32_t now_ms);

 private:
  AppCtx& ctx_;
  uint32_t grace_ms_;
  bool boot_stamped_ = false;
  uint32_t boot_ms_ = 0;
  bool connected_once_ = false;
  uint32_t connect_ms_ = 0;
  // The boot tail (FinishBoot: stop spinner, first render, buttons) is
  // deferred past STA-connect until the first data lands — the spinner
  // keeps spinning while the data source connects — or a timeout elapses.
  bool boot_finalized_ = false;
  bool ap_up_ = false;
};

}  // namespace btclock
