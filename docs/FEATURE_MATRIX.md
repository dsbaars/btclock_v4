# BTClock v4 — Feature Parity Matrix

Tracks every user-visible feature of the upstream Arduino/PlatformIO firmware
(`src/`) against BTClock v4. Use this as the single checklist when deciding
what to port next.

_Last updated: **2026-05-02**. This doc is hand-maintained — keep in sync when
features land._

**Status tokens**

- Implemented — feature is at parity (or intentionally scoped equivalent).
- Partial — functional but missing sub-behaviors; notes list the gap.
- Stubbed — endpoint/surface exists but does nothing useful (returns 501 or a
  placeholder value).
- Missing — no code exists yet.
- N/A — intentionally dropped (note why).

**State (2026-05-02):**

Host tests: 763/763 passing (11 256 assertions). REV_A/REV_B/V8 all
build clean on ESP-IDF v6.0.1 and both wired boards (Rev A + Rev B) have
been flashed + photographed against the 4.0.0-beta.6 tag. Big landings
since the 2026-04-26 snapshot:

- All settings PATCHes that touch main-task-owned state now go through
  the ControlCommand queue (`kRebuildScreens`, `kSetBlockFeeDec`,
  `kSetFont`, `kSetTimezone`). Closed a Rev B heap-corruption crash
  where `actCurrencies` PATCH dangling-referenced an in-flight Render's
  `current_currency()`.
- `satsVariant` shipped end-to-end: schema field (`uint 0..15`), live
  PATCH hook, 16-glyph contact sheet under
  [docs/img/fonts/sats_variants.png](img/fonts/sats_variants.png), and
  a visual picker in the WebUI Settings page.
- Rotation cursor persists across reboots: `current_slot` is written
  into a new `rt` NVS namespace on every change, restored on boot,
  and falls back to `rotation_sequence_[0]` when the saved slot is no
  longer in the active plan. Previously the device always booted on
  block-height regardless of `screenOrder`.
- WebUI Settings page split the rotation list into per-screen and
  per-currency sections, with a sticky save/cancel action bar.
- libFuzzer harnesses landed for `components/webserver/url_decode`
  and `components/nostr/parser` (clang-only, opt-in via
  `-DBTCLOCK_FUZZ=ON`).
- CI host-test pipeline gained ccache + Ninja so unchanged-source
  pushes drop from ~18 min to ~1 min on the sanitize job.

Earlier (since the 2026-04-23 snapshot):

- Mining-pool stack went from "ported, no orchestrator" to live: pool
  selector + WSS-driven catalog, on-demand HTTPS logo fetcher
  (LittleFS-cached, vendored bitmaps mostly dropped), three new pools
  (NerdMiner .org / .io, ViaBTC, Foundry USA), and a
  `POST /api/action/clear_pool_logos` admin endpoint.
- mempool.space + Kraken WSS source landed and is selected by
  `dataSource=1`; `dataSource=2` honours the `ceEndpoint` custom WS.
- Periodic full refresh (`fullRefreshMin`) and steal-focus on new
  block both shipped via the `RefreshPolicy` / `BlockEventPolicy`
  helpers.
- mDNS now advertises `_http._tcp` + `_btclock._tcp` from
  `main/app/boot/init_mdns.cpp`.
- WiFi long-outage reboot watchdog (`wpTimeout`) wired through
  `init_network.cpp`; LED-test-on-boot, inverse-buttons, and the
  frontlight runtime prefs (`flDisable`, `flMaxBrightness`,
  `flOffWhenDark`, `luxLightToggle`) are all now read from NVS.
- Screen rotation order (`screenOrder` CSV) is read at boot via
  `rotation_plan::BuildRotationSequence`; WebUI ships a drag-reorder.
- `POST /upload/firmware` and `POST /api/factory_reset` are no longer
  stubs.
- `POST /api/firmware/auto_update` now drives a TLS-gated pull from
  `gitReleaseUrl` (default flipped to the v4 Forgejo release feed),
  matches `btclock_<variant>_ota.bin` per board × panel, verifies the
  sibling `.sha256`, and reboots; release pipeline publishes the
  matching flat assets per variant with shared bins deduped.

DND/scheduling, peripherals, OTA, and provisioning are now at full
parity for the in-scope feature set.

---

## TL;DR summary

Rough per-category parity (counts full + partial as "done"):

| Category | Rows | Done | Parity |
|---|---:|---:|---:|
| Display / screens | 16 | 16 | 100% (1 partial: runtime-pushed countdown variant) |
| Data sources | 12 | 12 | 100% (2 partial: BTClock WS mempool sub-channels, WASM bindings) |
| HTTP / Control API | 40 | 40 | 100% |
| Provisioning / WiFi | 8 | 8 | 100% |
| LED + light subsystems | 9 | 9 | 100% |
| DND / scheduling | 4 | 4 | 100% |
| OTA / updates | 4 | 3 | 75% (1 N/A: ArduinoOTA push, replaced by `/upload/firmware`) |
| Peripherals | 6 | 6 | 100% |
| Persistence (NVS) | 6 | 6 | 100% |
| Build / board variants | 7 | 6 | ~86% |
| **Totals** | **112** | **110** | **~98%** |

10 of 11 rotation screens render (block, clock, halving, supply,
moscow, price, mcap, fee-rate, mining-pool hashrate/earnings, Bitaxe
hashrate/best-diff — runtime-pushed countdown variant still pending).
Frontlight is fully live on Rev B with fade + block-flash + BH1750
auto-off, all prefs NVS-backed. Mining-pool stack ships 11 pools
(NerdMiner .org/.io, ViaBTC, Foundry USA on top of the original 7),
the on-demand logo fetcher caches into LittleFS, and a pool-selection
orchestrator boots from the `miningPoolName` pref. Data sources:
BTClock WSS, mempool+kraken, custom-WS, Nostr (schnorr-verified), Bitaxe
HTTP, mining-pool HTTPS — all live. Pull-OTA is now end-to-end:
`POST /api/firmware/auto_update` follows `gitReleaseUrl` to the v4
Forgejo release, picks the `btclock_<variant>_ota.bin` asset that
matches the running board × panel, verifies SHA-256, and reboots.
Remaining stubs are limited to a couple of pref-driven sub-behaviours.

---

## Display / screens

Enum values from [`src/lib/system/shared.hpp`](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/shared.hpp)
lines 45-67; rotation catalog in
[`src/lib/system/config.cpp:45-68`](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/config.cpp).
Renderers under [`main/screens/`](../main/screens).

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| Block height (`SCREEN_BLOCK_HEIGHT`) | [screen_handler.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/ui/screen_handler.cpp) | [block_height.cpp](../main/screens/block_height.cpp) | Implemented | — |
| Sats per currency (`SCREEN_SATS_PER_CURRENCY`, Moscow-time) | screen_handler.cpp | [moscow_time.cpp](../main/screens/moscow_time.cpp) | Implemented (USD + EUR/GBP/JPY) | — |
| Sats-symbol variant picker (`satsVariant`) | (n/a in v3 — single hardcoded glyph) | Schema field (uint 0..15) + `ScreenManager::SetSatsVariant` runtime hook + WebUI Settings visual picker; renders one of 16 glyphs at U+E000..U+E00F of the SatoshiSymbol font on the moscow-time and nostr-zap screens. Live PATCH via `/api/settings`; contact sheet at [docs/img/fonts/sats_variants.png](img/fonts/sats_variants.png) | Implemented (superset) | — |
| BTC ticker (`SCREEN_BTC_TICKER`) | screen_handler.cpp | [btc_price.cpp](../main/screens/btc_price.cpp) | Implemented (multi-currency) | — |
| Time / clock (`SCREEN_TIME`) | screen_handler.cpp | [clock.cpp](../main/screens/clock.cpp) | Implemented (blocks-mode) | — |
| Halving countdown (`SCREEN_HALVING_COUNTDOWN`) | screen_handler.cpp | [halving.cpp](../main/screens/halving.cpp) | Implemented (blocks-mode + years/days/hours/mins time-mode via `as_blocks` flag) | — |
| Block fee rate (`SCREEN_BLOCK_FEE_RATE`) | screen_handler.cpp | [fee_rate.cpp](../main/screens/fee_rate.cpp) | Implemented (decimal sats/vB from `blockfee2` WS topic + paired-split-text "sat/vB" unit on the trailing panel) | — |
| Market cap (`SCREEN_MARKET_CAP`) | screen_handler.cpp | [market_cap.cpp](../main/screens/market_cap.cpp) | Implemented bigChars ($1.56T form) by default; small-char 3-digit-group mode exposed through panel_texts for /api/status, renderer still paints bigChars only | — |
| Bitcoin supply (`SCREEN_BITCOIN_SUPPLY`) | screen_handler.cpp | [bitcoin_supply.cpp](../main/screens/bitcoin_supply.cpp) | Implemented bigChars (19.9M) + supplyPercent (93.48 %) modes; small-char 3-digit-group mirror available via panel_texts, renderer pending | — |
| Mining pool hashrate / earnings (`SCREEN_MINING_POOL_STATS_*`) | screen_handler.cpp | [mining_pool.cpp](../main/screens/mining_pool.cpp) | Implemented (11 pools selectable via `miningPoolName` pref + on-demand LittleFS-cached logo fetcher) | `btclock_v4-5yi` |
| Bitaxe hashrate / best difficulty (`SCREEN_BITAXE_*`) | screen_handler.cpp | [bitaxe.cpp](../main/screens/bitaxe.cpp) | Implemented (HTTP poll + tail-aware unit glyph) | — |
| Runtime-pushed custom / text / countdown (`SCREEN_CUSTOM`, `SCREEN_COUNTDOWN`) | screen_handler.cpp, actions.cpp `/api/show/text` | [show_custom.cpp](../main/screens/show_custom.cpp) — /api/show/text + /api/show/custom wired; SCREEN_COUNTDOWN still missing | Partial (countdown variant pending) | — |
| User-configurable rotation order | [config.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/config.cpp) `rebuildScreenMappings`, default `DEFAULT_SCREEN_ORDER` | [rotation_plan.hpp](../main/app/rotation_plan.hpp) + `screen_manager.cpp` reads `screenOrder` CSV at boot; WebUI submodule ships drag-reorder UI split into per-screen + per-currency lists with a sticky save-cancel action bar | Implemented | — |
| Steal focus on new block (auto-flip to block screen) | `DEFAULT_STEAL_FOCUS`, ScreenHandler | [block_event_policy.hpp](../main/app/block_event_policy.hpp) + `screen_manager.cpp` flips to `kBlockHeight` on `ConsumeNewBlock` when `stealFocus` is set; respects override screens | Implemented | — |
| Full periodic refresh (EPD ghost clear) | `DEFAULT_MINUTES_FULL_REFRESH` | [refresh_policy.hpp](../main/app/refresh_policy.hpp) honoured by `screen_manager.cpp`; respects `refrScrnChange` + `fullRefreshMin` | Implemented | — |
| Screen visibility toggles | settings.cpp per-screen flags | [settings_api.cpp](../components/settings/settings_api.cpp) — PATCH `screens[].enabled` writes `screen<ID>Visible` NVS flags | Implemented (WebUI-level; runtime consumers pick up on next mapping rebuild) | — |

## Data sources

Old-firmware data-source modules live under
[`src/lib/data_sources/`](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources).
BTClock v4 uses a `DataHub` + `DataSource` abstraction
([`components/data_core`](../components/data_core)). Multiple concrete
sources exist; not all are wired into `main.cpp` yet.

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| DataHub / registry abstraction | [live_service.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/live_service.hpp) | [hub.hpp](../components/data_core/include/data_core/hub.hpp) | Implemented | — |
| BTClock WS v2 (`BTCLOCK_SOURCE`) | [v2_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/v2_notify.cpp) | [btclock_data.cpp](../components/btclock_data/btclock_data.cpp) | Partial: block + price + blockfee + blockfee2 subs; mempool sub-channels still missing | — |
| mempool.space third-party (`THIRD_PARTY_SOURCE`) | [block_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/block_notify.cpp), [price_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/price_notify.cpp) | [mempool_kraken_source.cpp](../main/sources/mempool_kraken_source.cpp) — independent mempool.space + Kraken WSS clients; selected by `dataSource=1` | Implemented | — |
| Nostr source + Zap notifier (`NOSTR_SOURCE`) | [nostr_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/nostr_notify.cpp) | [components/nostr/](../components/nostr) — relay client, REQ/EVENT sub, zap listener (kind 9735), NostrDataSource (kind 30078) with `d`-tag→snapshot dispatch + staleness gate; BIP-340 schnorr verification gates both consumers via [event_verify.cpp](../components/nostr/src/event_verify.cpp) (vendored libsecp256k1); zap callback wired through [init_zap_listener.cpp](../main/app/boot/init_zap_listener.cpp) to frontlight + LED + on-screen overlay | Implemented; NVS-gated init (requires `nostr/enable`+`relay`+`pub` keys set) | — |
| Custom WS endpoint (`CUSTOM_SOURCE`) | config.cpp `ceEndpoint` | [sources_uri.cpp](../main/sources/sources_uri.cpp) — `dataSource=2` honours `ceEndpoint` + `ceDisableSSL` for the BTClock-protocol client | Implemented | — |
| Kraken price fallback | price_notify.cpp | [mempool_kraken_source.cpp](../main/sources/mempool_kraken_source.cpp) — Kraken V2 ticker, one channel per active currency | Implemented | — |
| Multi-currency active set (`actCurrencies`) | config.cpp `getActiveCurrencies` | [sources.cpp](../main/sources/sources.cpp) reads `actCurrencies` CSV from NVS; PATCH `/api/settings` persists, validator rejects unknown codes | Implemented | — (sats symbol) |
| Bitaxe HTTP poll | [bitaxe_fetch.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/bitaxe_fetch.cpp) | [components/bitaxe/](../components/bitaxe) wired via `MakeBitaxeSource` in [sources.cpp](../main/sources/sources.cpp); cadence configurable via `bitaxePollMin` pref | Implemented | — |
| Mining pool stats (11 pools behind shared base + TLS gate) | [mining_pool/*](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/mining_pool) | [mining_pool_common/](../components/mining_pool_common) + per-pool components (braiins / ocean / noderunners / satoshi_radio / public_pool / ckpool+EU / gobrrr / nerdminer / viabtc / foundry) + [io/mining_pool_selector.cpp](../main/io/mining_pool_selector.cpp) reading `miningPoolName` pref; cadence configurable via `miningPoolPollMin` | Implemented | —, `-zxk`, `-zs3`, `-qmh`, `-g7h`, `-dhs`, `-9c1` |
| On-demand mining-pool logo fetcher | (n/a in v3 — bitmaps were inlined per-pool) | [components/pool_logo_fetcher/](../components/pool_logo_fetcher) — HTTPS fetch from `poolLogosUrl`, LittleFS cache under `/lfs/pool_logos/`, vendored fallback for offline boot | Implemented | `btclock_v4-5yi` |
| TLS gate (single-flight mbedtls handshake) | [tls_gate.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/tls_gate.hpp) | [components/tls_gate/](../components/tls_gate) | Implemented; wrapped around each mining-pool HTTPS handshake | — |
| Emscripten/WASM preview build (IDF-port helpers) | [lib/btclock/data_handler.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/lib/btclock/data_handler.cpp) (`__EMSCRIPTEN__`) | [tools/wasm/](../tools/wasm) — binding compiles `screens/common.cpp` + `screen_math.cpp` | Partial: 7 `parse*` bindings live (all on-device screens); pixel-accurate framebuffer + font rasterisation (Phase 2) in-flight | — |

## HTTP / Control API

Old firmware registers all routes via `server.on(...)` under
[`src/lib/net/webserver/`](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver).
BTClock v4 control routes live in
[`components/webserver/control_server.cpp`](../components/webserver/control_server.cpp).
Stub rows return HTTP 501 with a tracking token pointing at the
follow-up issue.

| Endpoint | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| `GET /api/status` | [status.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/status.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (`data[]` via [panel_texts.cpp](../main/screens/panel_texts.cpp), `leds[]` via `BuildLightsStatusArray`, real `dnd{}` via `DndIface`, real `timerRunning` via `TimerIface`) | — |
| `GET /api/system_status` | status.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (real FS fields via `btclock::GetLittleFsUsage`) | — |
| `GET /api/settings` | [settings.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/settings.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) + [components/settings/](../components/settings) | Implemented (58-field schema, NVS-backed, host-tested) | — |
| `PATCH /api/settings` | settings.cpp | [control_server.cpp](../components/webserver/control_server.cpp) + [components/settings/](../components/settings) | Implemented (partial-body, boot-only → `rebootRequired`) | — |
| `POST /api/full_refresh` | [actions.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/actions.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (event loop consumes `kFullRefresh` → `ScreenManager::MarkDirty`) | — |
| `POST /api/identify` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (fires `LedEffect::kIdentify` — rapid multi-colour flash) | — |
| `POST /api/restart` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/screen` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/currency` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/text` | actions.cpp | [control_server.cpp::HandleShowText](../components/webserver/control_server.cpp) — accepts `?t=` and `{"text":"..."}`, one char per panel uppercased | Implemented | — |
| `POST /api/show/custom` | (derived: custom screen) | [control_server.cpp::HandleShowCustom](../components/webserver/control_server.cpp) — bare array or `{"cells":[...]}`, verbatim per panel | Implemented | — |
| `POST /api/screen/next` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/screen/previous` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/action/pause` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → `ScreenManager::SetPaused(true)` via `TimerIface` | Implemented | — |
| `POST /api/action/timer_restart` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → un-pause + `ScreenManager::RestartTimer(now)` | Implemented | — |
| `POST /api/stop_datasources` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/restart_datasources` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/wifi_set_tx_power` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (range-validated quarter-dBm via `esp_wifi_set_max_tx_power`) | — |
| `GET /api/dnd/status` | [dnd.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/dnd.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) + [components/dnd/](../components/dnd) | Implemented (NVS-backed state, wall-clock active check, matches old JSON shape) | — |
| `POST /api/dnd/enable` | dnd.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → `Dnd::SetEnabled(true)` | Implemented | — |
| `POST /api/dnd/disable` | dnd.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → `Dnd::SetEnabled(false)` | Implemented | — |
| `GET /api/lights` | [lights.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/lights.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (per-pixel RGB + hex, reversed ordering matches old firmware) | — |
| `POST /api/lights/color` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (`?c=RRGGBB` or `?c=off`, echoes status body) | — |
| `POST /api/lights/off` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (sets solid 0, mutes strip) | — |
| `POST /api/lights/set` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (per-pixel JSON array, accepts hex or r/g/b) | — |
| `POST /api/frontlight/on` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (FrontlightIface → `FrontlightController::On`; 503 on boards without frontlight) | — |
| `POST /api/frontlight/off` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (FrontlightIface → `FrontlightController::Off`) | — |
| `POST /api/frontlight/flash` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (block-flash pulse) | — |
| `GET /api/frontlight/status` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (JSON `flStatus[]` is per-channel current 12-bit duty mirrored from `FrontlightController::channel_duties_`; reflects staggered-flash state mid-pulse). | — |
| `POST /api/frontlight/brightness` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (validated duty → fade) | — |
| `POST /api/frontlight/set` | — (v4-only) | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (per-channel 12-bit duty array; bypasses always_on / DND gates so a debug pattern persists until the next ambient transition / brightness PATCH / flash) | New in v4 — no v3 equivalent. |
| `POST /upload/firmware` | [ota_routes.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/ota_routes.cpp) | [control_server.cpp::HandleUploadFirmware](../components/webserver/control_server.cpp) → [components/ota/](../components/ota) | Implemented (streams body to inactive app partition, SHA-256 verify, reboots into new slot; PSRAM buffer; auth-gated) | — |
| `POST /upload/webui` | ota_routes.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → [`FlashWebuiImage`](../components/btclock_fs/littlefs.cpp) | Implemented (streams blob to `storage` partition, reboots; gated by HTTP Basic auth when `httpAuthEnabled`) | — |
| `POST /api/firmware/auto_update` | ota_routes.cpp | [control_server.cpp::HandleFirmwareAutoUpdate](../components/webserver/control_server.cpp) → [ota_manager.cpp::TriggerAutoUpdate](../components/ota/ota_manager.cpp) | Implemented (TLS-gated fetch of `gitReleaseUrl`, walks Forgejo `assets[]` for `btclock_<variant>_ota.bin` + `.sha256`, esp_https_ota stream, partition rehash + match, reboot; status surfaced via `/api/status.isOTAUpdating`) | — |
| `POST /api/factory_reset` | (WiFiManager reset) | [control_server.cpp::HandleFactoryReset](../components/webserver/control_server.cpp) → [`PerformFactoryReset`](../components/settings/factory_reset.cpp) | Implemented (confirmation-body gated, wipes NVS, reboots to provisioning) | — |
| `POST /api/action/simulate_zap` | (n/a in v3) | [control_server.cpp::HandleActionSimulateZap](../components/webserver/control_server.cpp) | Implemented (fires the zap pipeline as if a Nostr kind-9735 had arrived; useful for dev / WebUI QA) | — |
| `POST /api/action/clear_pool_logos` | (n/a in v3) | [control_server.cpp](../components/webserver/control_server.cpp) → wipes `/lfs/pool_logos/` so the on-demand fetcher re-pulls | Implemented | `btclock_v4-5yi` |
| SSE event stream (`/events`) | [webserver.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/webserver.cpp) | [sse_server.cpp](../components/webserver/sse_server.cpp) | Implemented (async `/events` chunked stream; welcome + `status` broadcasts on lights/DND/settings/timer mutations + DataHub updates + screen rotations; 5 s keep-alive comment frame; max 4 clients) | — |
| Static WebUI file server (`/`) | webserver.cpp `serveStatic` | [control_server.cpp `HandleStatic`](../components/webserver/control_server.cpp) | Implemented (gzip-aware, Cache-Control, 503 on FS-unmounted; intentionally ungated so the bundle can load before the first /api call triggers the Basic prompt) | — |
| HTTP Basic auth gate | webserver.cpp `requireHttpAuth` | [auth_gate.cpp](../components/webserver/auth_gate.cpp) | Implemented (every /api/* handler + SSE gated on `httpAuthEnabled`; constant-time compare; empty-pass lockout guard) | `btclock_v4-9x3` |
| Screen-order REST API | config.cpp + settings.cpp (3xh/36t closed) | PATCH `/api/settings` screens[] reorder: full-catalog requirement, dup/range validation, CSV persisted to `screenOrder` NVS; `ScreenManager` consumes via `rotation_plan::BuildRotationSequence` at boot + on PATCH | Implemented | — |

## Provisioning / WiFi

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| WiFiManager captive portal (first-boot AP) | WiFiManager library in main.cpp | [provisioning_server.cpp](../components/webserver/provisioning_server.cpp) + [boot_ui](../main/boot_ui.cpp) | Implemented | — |
| WPA2 AP with random password | (N/A — open AP historically) | main.cpp `MakeOrLoadApPassword` | Implemented (superset) | — |
| On-panel SSID + password + QR display | — | [provisioning_ui.cpp](../main/provisioning_ui.cpp) | Implemented (superset) | — |
| SSID scan API | (bundled in WiFiManager) | `GET /api/scan` (background scan on branch aa1d0cd8) | Implemented | — |
| `GET /api/version` (hw/fw/idf) | status.cpp (nested) | provisioning_server.cpp | Implemented | — |
| Captive-portal DNS hijack | WiFiManager | [dns_hijack.cpp](../components/webserver/dns_hijack.cpp) | Implemented | — |
| mDNS advertisement (`http._tcp`) | webserver.cpp `MDNS.begin(...)` | [init_mdns.cpp](../main/app/boot/init_mdns.cpp) advertises `_http._tcp` + `_btclock._tcp` against the configured hostname | Implemented | — |
| Auto-reconnect + 10-minute reboot on WiFi loss | main.cpp `checkWiFiConnection` | [wifi_guard.cpp](../main/io/wifi_guard.cpp) + `wpTimeout` watchdog in [init_network.cpp](../main/app/boot/init_network.cpp) (NVS-tunable; default 15 min → `esp_restart`) | Implemented | — |

## LED + light subsystems

Old firmware is in
[`src/lib/drivers/leds/led_handler.cpp`](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/drivers/leds/led_handler.cpp)
(3-in-1: NeoPixel state, DND, frontlight/PCA9685 fade). BTClock v4's
[`io/led_controller.cpp`](../main/io/led_controller.cpp) now carries the
production-path subset of the effect catalog plus NVS-backed prefs for
brightness / block-flash colour / disable / flash-on-update.

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| NeoPixel FreeRTOS task + queue | led_handler.cpp | led_controller.cpp | Implemented | — |
| Boot / idle / block-flash effects | led_handler.cpp `LED_FLASH_*` | led_controller.cpp | Implemented | — |
| Full effect catalog (production subset — identify, zap, heartbeat, data errors, WiFi states, flash-{success,error,update}, boot-failed, power-test) | led_handler.cpp `LED_*` constants | [led_controller.cpp](../main/io/led_controller.cpp) + [led_curves.cpp](../main/io/led_curves.cpp) | Implemented (progress-25/50/75/100 + start/pause-timer intentionally not ported — old firmware's setup-timer flow isn't carried over) | — |
| LED brightness + color + flash-on-update prefs (`DEFAULT_LED_BRIGHTNESS`, `BlockFlashColor`) | led_handler.cpp + defaults.hpp | led_controller.cpp — NVS namespace `"led"` keys `brightness`/`blockFlashCol`/`disable`/`flashUpdate` | Implemented | — |
| `DisableLeds` NVS toggle | defaults.hpp | led_controller.cpp — `"led"/"disable"` | Implemented | — |
| Frontlight PCA9685 channels init | led_handler.cpp `#ifdef HAS_FRONTLIGHT` | [frontlight_controller.cpp](../main/io/frontlight_controller.cpp) drives fade on boot | Implemented | — |
| Frontlight fade + flash-on-block + flash-on-zap | led_handler.cpp `frontlightFadeIn/OutAll` | [frontlight_controller.cpp](../main/io/frontlight_controller.cpp) — fade + block-flash wired from `ConsumeNewBlock`; zap-flash now triggered from [init_zap_listener.cpp](../main/app/boot/init_zap_listener.cpp) on Nostr kind-9735 | Implemented | — |
| Ambient-light auto-off (BH1750) | main.cpp `handleFrontlight` | [event_loop.cpp](../main/app/event_loop.cpp) feeds lux to `FrontlightController::OnAmbientLux`; threshold + enable persisted via `luxLightToggle` + `flOffWhenDark` NVS keys (read at boot + applied live on PATCH) | Implemented | — |
| NeoPixel zap-flash trigger wiring | led_handler.cpp `LED_EFFECT_NOSTR_ZAP` | [init_zap_listener.cpp](../main/app/boot/init_zap_listener.cpp) binds the on-zap callback into `LedController::Trigger(LedEffect::kZap)` plus the frontlight + on-screen overlay | Implemented | — |

## DND / scheduling

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| DND enabled flag (force-off LEDs) | led_handler.cpp `setDNDEnabled` | [components/dnd/](../components/dnd) + suppressor predicates on LED controller and frontlight controller | Implemented | — |
| Time-based DND window (start/end HH:MM) | led_handler.cpp `setDNDTimeRange`, [lib/dnd_window.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/lib/dnd_window.cpp) | [dnd_window.cpp](../components/dnd/dnd_window.cpp) (half-open + overnight wrap, host-tested) | Implemented | — |
| DND status surfaced in `/api/status` | status.cpp nested `dnd` object | control_server.cpp emits real `dnd{}` via `DndIface` → `Dnd::GetStatus` | Implemented | — |
| Screen-rotation timer active / pause | `DEFAULT_TIMER_ACTIVE`, actions.cpp | `ScreenManager::SetPaused/IsPaused/RestartTimer` + `TimerIface` wired to `/api/action/pause` and `/api/action/timer_restart` | Implemented | — |

## OTA / updates

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| Web-UI firmware upload (`U_FLASH`) | ota_routes.cpp | [control_server.cpp::HandleUploadFirmware](../components/webserver/control_server.cpp) → [components/ota/](../components/ota) | Implemented (PSRAM-buffered streaming, sequential writes, timeout retry) | — |
| Web-UI LittleFS upload (`U_SPIFFS`) | ota_routes.cpp | [control_server.cpp::HandleUploadWebui](../components/webserver/control_server.cpp) → [`FlashWebuiImage`](../components/btclock_fs/littlefs.cpp) | Implemented | — |
| Auto-update check (release feed) | ota_routes.cpp `onAutoUpdateFirmware`, `gitReleaseUrl` | [ota_manager.cpp::RunAutoUpdate](../components/ota/ota_manager.cpp) — fetches release JSON via TLS-gated HTTPS, parses Forgejo/GitHub `assets[]` for `<firmware_asset>` + `<firmware_asset>.sha256`, streams via `esp_https_ota`, rehashes the inactive partition and aborts on mismatch, then `esp_restart`. CI publishes per-variant `btclock_<variant>_ota.bin` assets so `MakeOtaConfig()` lookups resolve. | Implemented | — |
| ArduinoOTA push (PlatformIO → device) | [ota.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/ota/ota.cpp) | — (n/a — push-OTA replaced by `POST /upload/firmware`) | N/A | — |

## Peripherals

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| MCP23017 expander(s), incl. V8 dual-chip | [shared.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/shared.hpp) `mcp1/mcp2`, epd.cpp | [mcp23017](../components/mcp23017), [board_v8](../main/board/board_v8.hpp) | Implemented | — |
| Buttons (4 × tactile, click + long-press) | [button_handler.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/drivers/buttons/button_handler.cpp) | [buttons](../components/buttons) | Implemented | — |
| SSD1680 EPDs (2.13" + 2.9", shared SPI bus, shadow-FB partial refresh) | [epd.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/drivers/epd/epd.cpp) | [epd_ssd1680](../components/epd_ssd1680) | Implemented (native driver) | — |
| BH1750 ambient-light sensor | BH1750 Arduino lib | [bh1750](../components/bh1750) — feeds frontlight auto-off through `event_loop.cpp` | Implemented | — |
| PCA9685 frontlight driver | PCA9685 Arduino lib | [pca9685](../components/pca9685) + `frontlight_controller.cpp` (fade + block-flash + zap-flash) | Implemented | — |
| Inverse-buttons pref (`InverseButtons`) | button_handler.cpp | [main/sources/sources.cpp:189](../main/sources/sources.cpp) reads `kInverseButtons` and swaps button-1↔button-N at post time | Implemented | `btclock_v4-7da` |

## Persistence (NVS / settings)

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| NVS wrapper | Arduino `Preferences` | [prefs](../components/prefs) | Implemented | — |
| `PrefKeys::*` catalog (~80 keys, see [pref_keys.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/pref_keys.hpp)) | pref_keys.hpp | [components/settings/include/settings/pref_keys.hpp](../components/settings/include/settings/pref_keys.hpp) — 77 keys under namespace `settings` (+ `kLastSlot` under the new `rt` namespace), 15-char limit statically enforced via `BTCLOCK_PREF_KEY_ASSERT` | Implemented | — |
| Settings schema + PATCH validation | settings.cpp `onApiSettingsPatch` | [settings_api.cpp](../components/settings/settings_api.cpp) — typed field table, boot-only classification, range + enum validation | Implemented | — |
| Screen-order NVS + catalog merge | config.cpp `rebuildScreenMappings`, `DEFAULT_SCREEN_ORDER` | [rotation_plan.hpp](../main/app/rotation_plan.hpp) parses `screenOrder` CSV + merges with `screen<id>Visible` toggles + the active-currency catalog; consumed at boot by `screen_manager.cpp` | Implemented | — |
| Factory-reset / "erase settings" flow | WiFiManager reset path | [factory_reset.cpp](../components/settings/factory_reset.cpp) — `PerformFactoryReset()` wipes NVS + reboots; reachable via `POST /api/factory_reset` (confirmation-gated) | Implemented | — |
| Last-shown rotation slot resumes across reboots | (n/a in v3 — always booted on slot 0) | New `rt` NVS namespace (kept off `kSettingsNs`), key `kLastSlot`; written by `PublishStatus` on every slot change, read by `init_screen_manager.cpp` after `SetRotationSequence`, restored via `SetSlot` if still valid; falls back to `rotation_sequence_[0]` when not (e.g. screenOrder change between reboots) | Implemented (superset) | — |

## Build / board variants

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| Rev A (Lolin S3 mini) | [platformio.ini](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/platformio.ini) `env:lolin_s3_mini*` | [board_rev_a.hpp](../main/board/board_rev_a.hpp), `-DBTCLOCK_BOARD=REV_A` | Implemented | — |
| Rev B | platformio.ini `env:btclock_rev_b*` | [board_rev_b.hpp](../main/board/board_rev_b.hpp) (default) | Implemented | — |
| V8 (16 MB, dual MCP) | platformio.ini `env:btclock_v8*` | [board_v8.hpp](../main/board/board_v8.hpp) | Implemented | — |
| 2.13" EPD (GDEY0213B74) | `-D VERSION_EPD_2_13` | `-DBTCLOCK_PANEL=2_13` (default — not a constraint; any board × any panel configures) | Implemented | — |
| 2.9" EPD (GDEY029T94) | `-D VERSION_EPD_2_9` | `-DBTCLOCK_PANEL=2_9` (validated on Rev A; other boards configure but un-flashed) | Implemented | — |
| 7.5" EPD (GDEY075T7, UC8179) | n/a | `-DBTCLOCK_PANEL=7_5` (scaffold only — un-flashed; intended for Rev B) | Stubbed | — |
| CI matrix (4 env builds) | `.gitea/workflows` | [.forgejo/workflows/host_tests.yaml](../.forgejo/workflows/host_tests.yaml) on push, [release.yaml](../.forgejo/workflows/release.yaml) on tag (host_tests → webui_and_lfs → firmware matrix [rev-a, rev-a-29, rev-b, v8] → release) | Implemented | — |

---

## How to use this doc

Treat this as a ground-truth checklist, not a history log. When you land a
feature, flip its row here (Missing/Stubbed → Partial →
Implemented) in the same PR, and drop the tracking link when the beads issue
closes. Beads issues remain the source of truth for in-flight work — this
matrix just collects them in one scannable place. If you discover a feature
gap the table misses, add a row; if you're unsure whether it's a distinct
feature, fold it into the nearest parent row as a sub-bullet rather than
inventing a new one.
