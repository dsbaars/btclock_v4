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
// Also installs the SetOnUpdate → xTaskNotifyGive fan-in and blocks
// for up to 30 s waiting on the first block-height snapshot so the
// initial render has a real frame to paint. The button reader comes
// up after the first paint so early clicks don't race a blank
// display.
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
// the notify, wait for first snapshot, paint, start buttons.
void WireDataSources(AppCtx& ctx);

// Pure helper exposed so host tests can pin the URI shape without
// dragging in NVS. Maps the v3 dataSource enum onto an actual WSS
// endpoint:
//   * 0 (BTCLOCK_SOURCE)         -> wss://ws.btclock.dev/api/v2/ws
//   * 2 (CUSTOM)                 -> ws[s]://<endpoint>/api/v2/ws
//                                   (wss unless disable_ssl is true).
//                                   Any leading ws:// or wss:// in
//                                   `endpoint` is stripped first.
//   * 1, 3 (mempool+kraken)      -> falls back to BTCLOCK_SOURCE; the
//                                   mempool+kraken polling source
//                                   isn't implemented yet (bd-1xc).
std::string BuildBtclockSourceUri(std::uint8_t data_source,
                                  const std::string& endpoint,
                                  bool disable_ssl);

}  // namespace btclock
