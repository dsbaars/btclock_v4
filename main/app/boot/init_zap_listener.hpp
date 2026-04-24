// Nostr zap-receipt listener stack.
//
// A separate WSS connection from the Nostr DataSource so enabling/
// disabling one doesn't tear down the other and the zap relay URL
// can differ from the data relay. Reads NVS (namespace "nostr" for
// the master switch / URL / pubkey / LED-flash gate, namespace
// "frontlight" for the frontlight-flash gate, namespace "settings"
// for the notification-screen gates) once at boot; the resulting
// atomics live on AppCtx so a future /api/settings PATCH can flip
// them without tearing down the listener.
//
// On each zap receipt the worker-thread callback:
//   * Patches DataSnapshot::latest_zap so /api/status echoes it.
//   * Conditionally posts the LED and frontlight flash effects.
//   * Raises ctx.zap_notify_pending and notifies the main task so the
//     render loop can flip ScreenManager into the kNostrZap overlay.
//
// Must be called after WireDataSources (needs ctx.hub) and only in
// STA mode (needs Wi-Fi). No-op in AP mode.

#pragma once

namespace btclock {

struct AppCtx;

void InitZapListener(AppCtx& ctx);

}  // namespace btclock
