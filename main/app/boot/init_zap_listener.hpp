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

#include <cctype>
#include <string_view>

namespace btclock {

struct AppCtx;

// Decide whether the zap listener can ride the Nostr data source's
// existing WSS connection (i.e. NIP-01 multi-subscription on one
// socket) instead of opening a second RelayClient. True when both URLs
// resolve to the same host:port + scheme after normalisation.
//
// Sharing collapses ~30+ KB of internal SRAM (a second 12 KB WS task
// stack + 8 KB rx buffer + mbedTLS context per WSS) and the matching
// largest-block fragmentation that was breaking the EPD render path
// on long-running devices (bd btclock_v4-17r). The fragmentation
// signal — `espLargestFreeBlock` pinned at 7 KB with two RelayClients
// vs ~31 KB with one — was the smoking gun.
//
// Normalisation rules, kept narrow on purpose:
//   * lowercase the scheme + host portion (URLs are case-insensitive
//     for both per RFC 3986 §3.1 / §3.2.2).
//   * strip a single trailing '/' from the path (relay roots are
//     advertised both with and without it; "wss://relay/" and
//     "wss://relay" must match).
//   * ignore empty-string inputs (return false; no socket to share).
//
// Deliberately NOT normalised: query strings, fragments, port numbers
// (the WS lib treats wss://host vs wss://host:443 as different anyway,
// and our schema rejects non-default ports today).
inline bool ShouldShareNostrRelay(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) return false;
  auto strip_slash = [](std::string_view s) {
    if (!s.empty() && s.back() == '/') s.remove_suffix(1);
    return s;
  };
  a = strip_slash(a);
  b = strip_slash(b);
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto la =
        static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const auto lb =
        static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (la != lb) return false;
  }
  return true;
}

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
