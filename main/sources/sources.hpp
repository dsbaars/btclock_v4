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
// Also installs the SetOnUpdate → xTaskNotifyGive fan-in. The wait
// for the first block-height snapshot, the first render, and the
// button reader bring-up are split off into FinishWiringDataSources
// so the HTTP control API can come up *before* the up-to-30-s data
// wait — that wait used to gate the webserver behind first-data, and
// users observed the device as "unresponsive over HTTP until the
// screen lit up". WireDataSources now returns as soon as the source
// tasks are spawned (fast); FinishWiringDataSources runs after
// InitControlApi to finish the boot.
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

// Do nothing in AP (provisioning) mode — the provisioning render was
// already painted by main. STA mode: build hub, attach sources, wire
// the notify, kick off StartAll, return. No blocking wait, no first
// render, no button bring-up — those are deferred to
// FinishWiringDataSources so the webserver can start in between.
void WireDataSources(AppCtx& ctx);

// STA-mode tail of the boot sequence. Blocks until the first
// blockheight snapshot lands (or 30 s elapses, whichever first),
// paints the first frame with whatever data is available, then
// constructs and starts the button reader. Idempotent against
// AP mode: returns immediately when ctx.hub is null (the AP boot
// path skips WireDataSources, so hub stays unset).
void FinishWiringDataSources(AppCtx& ctx);

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
