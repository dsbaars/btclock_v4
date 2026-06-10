// STA-mode data-source wiring.
//
// Builds the DataHub and attaches every producer that feeds the
// snapshot the screen renderers consume:
//   * BtclockDataSource — the public v2/ws feed (blockheight, price,
//     fee, etc.).
//   * (optional) NostrDataSource — the same payload over Nostr relays
//     when the user has configured a publisher.
//   * (optional) Mining-pool HTTPS poller — selected pool only.
//   * (optional) Bitaxe LAN poller.
//
// Also installs the SetOnUpdate → xTaskNotifyGive fan-in. WireDataSources
// returns as soon as the source tasks are spawned (fast). The first
// render and button bring-up live in FinishBoot, which runs
// unconditionally at boot so the device is usable before/without a
// connection; WireDataSources itself is deferred to the first STA connect
// (NetworkCoordinator), since a non-blocking boot has no connection — and
// no reachable upstream currency catalogue — until then.
//
// Zap listener lives in app/boot/init_zap_listener — separate WSS
// connection from the Nostr DataSource so enabling/disabling one
// doesn't tear down the other.
//
// Caller must have already constructed ctx.sm and ctx.panels and
// populated ctx.currencies, ctx.button_q, ctx.main_task.

#pragma once

#include <cstdint>
#include <string>

namespace btclock {

struct AppCtx;

// Build the hub, attach sources, wire the update notify, kick off
// StartAll, return. No blocking wait. Called by NetworkCoordinator on the
// first STA connect (boot is non-blocking, so there's no connection — and
// no point fetching the upstream currency catalogue — until then). Bails
// in AP mode as a safety net; the coordinator only calls it once STA holds
// an IP and any fallback portal has been torn down.
void WireDataSources(AppCtx& ctx);

// Deferred companion to WireDataSources, called by NetworkCoordinator on
// the first STA connect: fetches the upstream `/api/v2/currencies`
// catalogue (a blocking HTTPS GET that can't run during the non-blocking
// boot) and prunes the active currency rotation to what the backend
// serves, re-subscribing the v2 source via SetCurrencies. No-op for
// dataSource=1 (mempool+kraken) or when no v2 source is wired.
void RefreshUpstreamCurrencies(AppCtx& ctx);

// Unconditional tail of the boot sequence (replaces the old, hub-gated
// FinishWiringDataSources). Drops the boot spinner, paints a first frame
// (placeholders when the hub isn't wired yet), and brings up the button
// reader so the device is usable even before — or without — a network
// connection. Returns early in pure-provisioning mode (the portal UI owns
// the panels and there are no buttons).
void FinishBoot(AppCtx& ctx);

// Pure helper exposed so host tests can pin the URI shape without
// dragging in NVS. Maps the dataSource enum (shared with WebUI's
// DataSourceType in data/src/lib/types/settings.ts) onto an actual WSS
// endpoint:
//   * 0 (BTCLOCK_SOURCE)         -> wss://ws.btclock.dev/api/v2/ws
//   * 1 (THIRD_PARTY_SOURCE)     -> falls back to BTCLOCK_SOURCE; the
//                                   mempool+kraken source uses its own
//                                   hard-coded URIs (not v2/ws shaped),
//                                   so this helper is only consulted on
//                                   the v2 path. WireDataSources skips
//                                   it entirely for ds=1.
//   * 2 (NOSTR_SOURCE)           -> ws[s]://<endpoint>/api/v2/ws when
//                                   ceEndpoint is set (Nostr is enabled
//                                   via its own settings; a non-empty
//                                   ceEndpoint here means the user wants
//                                   a custom price feed alongside Nostr),
//                                   else the public default.
//   * 3 (CUSTOM_SOURCE)          -> ws[s]://<endpoint>/api/v2/ws
//                                   (wss unless disable_ssl is true).
//                                   Any leading ws:// or wss:// in
//                                   `endpoint` is stripped first. Same
//                                   branch as ds=2 — both share the
//                                   custom-endpoint path.
std::string BuildBtclockSourceUri(std::uint8_t data_source,
                                  const std::string& endpoint,
                                  bool disable_ssl);

}  // namespace btclock
