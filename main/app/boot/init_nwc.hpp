// NWC (NIP-47) wallet client bring-up.
//
// Single dedicated RelayClient + SubscriptionManager + NwcClient.
// Reads NVS (canonical "settings" namespace): kNwcUri, kNwcEnabled,
// kNwcRefreshSecs, kNwcFlashOnPay. The boot path constructs the
// client once; PATCH-toggling nwcEnabled or rotating the URI requires
// a reboot (the WSS task + signer state caches are wired one-shot).
// nwcRefreshSecs and nwcFlashOnPay are runtime-editable via
// RefreshNwcSettings.
//
// On each callback:
//   * SetOnReady — log encryption variant + advertised methods; fire
//     the first RequestGetBalance() so the screen has a value before
//     the periodic timer ticks.
//   * SetOnBalance — patch DataSnapshot::nwc_balance_msat through
//     DataHub. Debounces an NVS write of kNwcLastBalSat (capped at
//     u32 sats) so the next boot can show a stale value while the
//     fresh balance is in flight.
//   * SetOnPayment — patch DataSnapshot::nwc_last_payment + raise
//     ctx.nwc_notify_pending so the event-loop pops the overlay.
//
// Refresh tick: esp_timer-driven periodic call into
// NwcClient::RequestGetBalance(). Paused while NwcClient isn't kReady;
// reprimed when SetOnReady fires.
//
// Must run after WireDataSources (needs ctx.hub) and only in STA mode
// (needs Wi-Fi). No-op in AP mode.

#pragma once

namespace btclock {

struct AppCtx;

void InitNwc(AppCtx& ctx);

// Re-read the runtime-editable NWC prefs. The boot-only fields
// (kNwcUri, kNwcEnabled) are not touched — the schema's
// rebootRequired flag walks the user through that path. Pulls the
// new refresh interval into the ESP timer + reloads the flash gate
// atomic so a PATCH lands live.
void RefreshNwcSettings(AppCtx& ctx);

}  // namespace btclock
