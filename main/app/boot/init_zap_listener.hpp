// Nostr zap-receipt listener stack.
//
// A separate WSS connection from the Nostr DataSource so enabling/
// disabling one doesn't tear down the other and the zap relay URL
// can differ from the data relay. Reads NVS (canonical "settings"
// namespace where /api/settings PATCH writes — kNostrRelay,
// kNostrZapPubkey, kNostrZapNotify, kLedFlashOnZap, kFlFlashOnZap,
// kScrnRestoreZap) once at boot; the resulting atomics live on
// AppCtx so a /api/settings PATCH can flip them without tearing down
// the listener (RefreshZapListenerSettings handles that path).
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

// Re-read the runtime-editable zap-listener prefs from the canonical
// "settings" NVS namespace and refresh the in-memory atomics. When the
// listener is already wired and the master toggle stays on, the
// SubscriptionManager is stopped + restarted so a new zap pubkey
// actually re-subscribes on the relay (additive REQ frames alone
// would leave the old pubkey's stream open). No-op when the listener
// hasn't been constructed (e.g. boot disabled it). Wired into the
// control server's on_nostr_changed hook so a PATCH lands without
// reboot. bd btclock_v4-aw5 / btclock_v4-q1l.
void RefreshZapListenerSettings(AppCtx& ctx);

}  // namespace btclock
