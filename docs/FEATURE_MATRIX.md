# BTClock v4 — Feature Parity Matrix

Tracks every user-visible feature of the upstream Arduino/PlatformIO firmware
(`src/`) against BTClock v4. Use this as the single checklist when deciding
what to port next.

_Last updated: **2026-04-23**. This doc is hand-maintained — keep in sync when
features land._

**Status tokens**

- Implemented — feature is at parity (or intentionally scoped equivalent).
- Partial — functional but missing sub-behaviors; notes list the gap.
- Stubbed — endpoint/surface exists but does nothing useful (returns 501 or a
  placeholder value).
- Missing — no code exists yet.
- N/A — intentionally dropped (note why).

**State (2026-04-23):**

Host tests: 96/96 passing (1488 assertions). REV_A/REV_B/V8 all build
clean and both boards have been flashed + photographed.

---

## TL;DR summary

Rough per-category parity (counts full + partial as "done"):

| Category | Rows | Done | Parity |
|---|---:|---:|---:|
| Display / screens | 15 | 8 | ~53% |
| Data sources | 11 | 4 | ~36% |
| HTTP / Control API | 34 | 19 | ~56% |
| Provisioning / WiFi | 8 | 8 | 100% |
| LED + light subsystems | 8 | 5 | ~63% |
| DND / scheduling | 4 | 0 | 0% |
| OTA / updates | 4 | 0 | 0% |
| Peripherals | 6 | 5 | ~83% |
| Persistence (NVS) | 5 | 1 | ~20% |
| Build / board variants | 5 | 5 | 100% |
| **Totals** | **100** | **55** | **~55%** |

8 of 11 rotation screens render (block, clock, halving,
supply, moscow, price, mcap, fee-rate — missing: mining-pool screen,
Bitaxe screen, runtime-pushed text/custom). Frontlight is fully live on
Rev B with fade + block-flash + BH1750 auto-off. The control API has
11+ endpoints functional; most stubs remain on DND/lights/OTA/settings.
Mining pools: all 7 DataSource ports landed behind the TLS gate, but
none wired into `main.cpp` yet (awaiting pool-selection orchestrator +
NVS pref). Nostr component present with relay client + zap listener +
blockfee2 subscription; NostrDataSource event→snapshot dispatch in
flight.

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
| BTC ticker (`SCREEN_BTC_TICKER`) | screen_handler.cpp | [btc_price.cpp](../main/screens/btc_price.cpp) | Implemented (multi-currency) | — |
| Time / clock (`SCREEN_TIME`) | screen_handler.cpp | [clock.cpp](../main/screens/clock.cpp) | Implemented (blocks-mode) | `btclock_v3_fci-lx0.13` |
| Halving countdown (`SCREEN_HALVING_COUNTDOWN`) | screen_handler.cpp | [halving.cpp](../main/screens/halving.cpp) | Implemented (blocks-mode + years/days/hours/mins time-mode via `as_blocks` flag) | `btclock_v3_fci-lx0.14` |
| Block fee rate (`SCREEN_BLOCK_FEE_RATE`) | screen_handler.cpp | [fee_rate.cpp](../main/screens/fee_rate.cpp) | Partial: integer sats/vB only; no "sat/vB" unit glyph; blockfee2 decimal variant pending | `btclock_v3_fci-wbr`, `btclock_v3_fci-znf` |
| Market cap (`SCREEN_MARKET_CAP`) | screen_handler.cpp | [market_cap.cpp](../main/screens/market_cap.cpp) | Implemented bigChars ($1.56T form) by default; small-char 3-digit-group mode exposed through panel_texts for /api/status, renderer still paints bigChars only | `btclock_v3_fci-33e` |
| Bitcoin supply (`SCREEN_BITCOIN_SUPPLY`) | screen_handler.cpp | [bitcoin_supply.cpp](../main/screens/bitcoin_supply.cpp) | Implemented bigChars (19.9M) + supplyPercent (93.48 %) modes; small-char 3-digit-group mirror available via panel_texts, renderer pending | `btclock_v3_fci-33e` |
| Mining pool hashrate / earnings (`SCREEN_MINING_POOL_STATS_*`) | screen_handler.cpp | — | Missing (7 data sources landed; renderer pending) | `btclock_v3_fci-lcw.1` |
| Bitaxe hashrate / best difficulty (`SCREEN_BITAXE_*`) | screen_handler.cpp | — | Missing | `btclock_v3_fci-lcw.2` |
| Runtime-pushed custom / text / countdown (`SCREEN_CUSTOM`, `SCREEN_COUNTDOWN`) | screen_handler.cpp, actions.cpp `/api/show/text` | [show_custom.cpp](../main/screens/show_custom.cpp) — /api/show/text + /api/show/custom wired; SCREEN_COUNTDOWN still missing | Partial (countdown variant pending) | `btclock_v3_fci-odc` |
| User-configurable rotation order | [config.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/config.cpp) `rebuildScreenMappings`, default `DEFAULT_SCREEN_ORDER` | — | Missing | `btclock_v3_fci-jek` |
| Steal focus on new block (auto-flip to block screen) | `DEFAULT_STEAL_FOCUS`, ScreenHandler | Partial: `ConsumeNewBlock` drives LED flash only | Partial | `btclock_v3_fci-wn6` |
| Full periodic refresh (EPD ghost clear) | `DEFAULT_MINUTES_FULL_REFRESH` | — | Missing | `btclock_v3_fci-6fi` |
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
| BTClock WS v2 (`BTCLOCK_SOURCE`) | [v2_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/v2_notify.cpp) | [btclock_data.cpp](../components/btclock_data/btclock_data.cpp) | Partial: block + price + blockfee + blockfee2 subs; mempool sub-channels still missing | `btclock_v3_fci-lcw` |
| mempool.space third-party (`THIRD_PARTY_SOURCE`) | [block_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/block_notify.cpp), [price_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/price_notify.cpp) | — | Missing | `btclock_v3_fci-lcw` |
| Nostr source + Zap notifier (`NOSTR_SOURCE`) | [nostr_notify.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/nostr_notify.cpp) | [components/nostr/](../components/nostr) — relay client, REQ/EVENT sub, zap listener (kind 9735), NostrDataSource (kind 30078) with `d`-tag→snapshot dispatch + staleness gate | Partial: signature verification deliberately omitted (listen-only, `TODO(nostr-sig)`); NVS-gated init (requires `nostr/enable`+`relay`+`pub` keys set); zap→frontlight trigger not yet wired | `btclock_v3_fci-0wm` |
| Custom WS endpoint (`CUSTOM_SOURCE`) | config.cpp `ceEndpoint` | — | Missing | `btclock_v3_fci-lcw` |
| Kraken price fallback | price_notify.cpp | — | Missing | `btclock_v3_fci-lcw` |
| Multi-currency active set (`actCurrencies`) | config.cpp `getActiveCurrencies` | Hardcoded `{USD,EUR,GBP,JPY}` in main.cpp; PATCH `/api/settings` persists `actCurrencies` CSV pref, validator rejects unknown codes | Partial: main.cpp needs to honour the NVS value on startup | `btclock_v3_fci-lx0.11` (sats symbol) |
| Bitaxe HTTP poll | [bitaxe_fetch.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/bitaxe_fetch.cpp) | — | Missing | `btclock_v3_fci-lcw` |
| Mining pool stats (7 pools behind shared base + TLS gate) | [mining_pool/*](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/data_sources/mining_pool) | [mining_pool_common/](../components/mining_pool_common) + per-pool components (braiins / ocean / noderunners / satoshi_radio / public_pool / ckpool+EU / gobrrr) | Partial: all 7 DataSources ported and host-tested; no main.cpp wiring; pool-selection orchestrator + NVS pref pending | `btclock_v3_fci-093`, `-zxk`, `-zs3`, `-qmh`, `-g7h`, `-dhs`, `-9c1` |
| TLS gate (single-flight mbedtls handshake) | [tls_gate.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/tls_gate.hpp) | [components/tls_gate/](../components/tls_gate) | Implemented; wrapped around each mining-pool HTTPS handshake | `btclock_v3_fci-3kt` |
| Emscripten/WASM preview build (IDF-port helpers) | [lib/btclock/data_handler.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/lib/btclock/data_handler.cpp) (`__EMSCRIPTEN__`) | [tools/wasm/](../tools/wasm) — binding compiles `screens/common.cpp` + `screen_math.cpp` | Partial: 7 `parse*` bindings live (all on-device screens); pixel-accurate framebuffer + font rasterisation (Phase 2) in-flight | `btclock_v3_fci-90q` |

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
| `POST /api/show/text` | actions.cpp | [control_server.cpp::HandleShowText](../components/webserver/control_server.cpp) — accepts `?t=` and `{"text":"..."}`, one char per panel uppercased | Implemented | `btclock_v3_fci-odc` |
| `POST /api/show/custom` | (derived: custom screen) | [control_server.cpp::HandleShowCustom](../components/webserver/control_server.cpp) — bare array or `{"cells":[...]}`, verbatim per panel | Implemented | `btclock_v3_fci-odc` |
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
| `GET /api/frontlight/status` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (JSON: enabled/duty/target/threshold/auto-off) | — |
| `POST /api/frontlight/brightness` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (validated duty → fade) | — |
| `POST /upload/firmware` | [ota_routes.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/ota_routes.cpp) | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| `POST /upload/webui` | ota_routes.cpp | [control_server.cpp](../components/webserver/control_server.cpp) → [`FlashWebuiImage`](../components/btclock_fs/littlefs.cpp) | Implemented (streams blob to `storage` partition, reboots; gated by HTTP Basic auth when `httpAuthEnabled`) | `btclock_v3_fci-5b2` |
| `POST /api/firmware/auto_update` | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| SSE event stream (`/events`) | [webserver.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/webserver/webserver.cpp) | [sse_server.cpp](../components/webserver/sse_server.cpp) | Implemented (async `/events` chunked stream; welcome + `status` broadcasts on lights/DND/settings/timer mutations + DataHub updates + screen rotations; 5 s keep-alive comment frame; max 4 clients) | `btclock_v3_fci-0sg` |
| Static WebUI file server (`/`) | webserver.cpp `serveStatic` | [control_server.cpp `HandleStatic`](../components/webserver/control_server.cpp) | Implemented (gzip-aware, Cache-Control, 503 on FS-unmounted; intentionally ungated so the bundle can load before the first /api call triggers the Basic prompt) | `btclock_v3_fci-equ` |
| HTTP Basic auth gate | webserver.cpp `requireHttpAuth` | [auth_gate.cpp](../components/webserver/auth_gate.cpp) | Implemented (every /api/* handler + SSE gated on `httpAuthEnabled`; constant-time compare; empty-pass lockout guard) | `btclock_v4-9x3` |
| Screen-order REST API | config.cpp + settings.cpp (3xh/36t closed) | PATCH `/api/settings` screens[] reorder: full-catalog requirement, dup/range validation, CSV persisted to `screenOrder` NVS | Partial: on-device `ScreenManager` doesn't yet read `screenOrder` back at boot | `btclock_v3_fci-jek` |

## Provisioning / WiFi

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| WiFiManager captive portal (first-boot AP) | WiFiManager library in main.cpp | [provisioning_server.cpp](../components/webserver/provisioning_server.cpp) + [boot_ui](../main/boot_ui.cpp) | Implemented | — |
| WPA2 AP with random password | (N/A — open AP historically) | main.cpp `MakeOrLoadApPassword` | Implemented (superset) | — |
| On-panel SSID + password + QR display | — | [provisioning_ui.cpp](../main/provisioning_ui.cpp) | Implemented (superset) | — |
| SSID scan API | (bundled in WiFiManager) | `GET /api/scan` (background scan on branch aa1d0cd8) | Implemented | — |
| `GET /api/version` (hw/fw/idf) | status.cpp (nested) | provisioning_server.cpp | Implemented | — |
| Captive-portal DNS hijack | WiFiManager | [dns_hijack.cpp](../components/webserver/dns_hijack.cpp) | Implemented | — |
| mDNS advertisement (`http._tcp`) | webserver.cpp `MDNS.begin(...)` | — | Missing | `btclock_v3_fci-equ` |
| Auto-reconnect + 10-minute reboot on WiFi loss | main.cpp `checkWiFiConnection` | [wifi_guard.cpp](../main/io/wifi_guard.cpp) (block-until-connected only) | Partial: no long-outage reboot timer | `btclock_v3_fci-79f` |

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
| Full effect catalog (production subset — identify, zap, heartbeat, data errors, WiFi states, flash-{success,error,update}, boot-failed, power-test) | led_handler.cpp `LED_*` constants | [led_controller.cpp](../main/io/led_controller.cpp) + [led_curves.cpp](../main/io/led_curves.cpp) | Implemented (progress-25/50/75/100 + start/pause-timer intentionally not ported — old firmware's setup-timer flow isn't carried over) | `btclock_v3_fci-fxh` |
| LED brightness + color + flash-on-update prefs (`DEFAULT_LED_BRIGHTNESS`, `BlockFlashColor`) | led_handler.cpp + defaults.hpp | led_controller.cpp — NVS namespace `"led"` keys `brightness`/`blockFlashCol`/`disable`/`flashUpdate` | Implemented | — |
| `DisableLeds` NVS toggle | defaults.hpp | led_controller.cpp — `"led"/"disable"` | Implemented | — |
| Frontlight PCA9685 channels init | led_handler.cpp `#ifdef HAS_FRONTLIGHT` | [frontlight_controller.cpp](../main/io/frontlight_controller.cpp) drives fade on boot | Implemented | `btclock_v3_fci-7ma` |
| Frontlight fade + flash-on-block + flash-on-zap | led_handler.cpp `frontlightFadeIn/OutAll` | frontlight_controller.cpp — fade + block-flash wired from `ConsumeNewBlock`; zap-flash event exposed but no Nostr source to trigger it yet | Partial: zap-flash awaits Nostr source (`btclock_v3_fci-lcw`) | `btclock_v3_fci-7ma` |
| Ambient-light auto-off (BH1750) | main.cpp `handleFrontlight` | main.cpp heartbeat feeds lux to `FrontlightController::OnAmbientLux`; threshold runtime-configurable, NVS persistence deferred | Partial: no NVS persistence of threshold/enable | `btclock_v3_fci-7ma`, `btclock_v3_fci-jwz` |
| NeoPixel zap-flash trigger wiring | led_handler.cpp `LED_EFFECT_NOSTR_ZAP` | led_controller.cpp exposes `LedEffect::kZap`; call site in main.cpp stubbed with `TODO(zap-wiring)` | Partial: effect ported, awaits `ZapListener` callback in nostr init path | `btclock_v3_fci-fxh`, `btclock_v3_fci-lcw` |

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
| Web-UI firmware upload (`U_FLASH`) | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| Web-UI LittleFS upload (`U_SPIFFS`) | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| Auto-update check (GitHub releases) | ota_routes.cpp `onAutoUpdateFirmware`, `gitReleaseUrl` | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| ArduinoOTA push (PlatformIO → device) | [ota.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/net/ota/ota.cpp) | — | Missing | `btclock_v3_fci-5b2` |

## Peripherals

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| MCP23017 expander(s), incl. V8 dual-chip | [shared.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/shared.hpp) `mcp1/mcp2`, epd.cpp | [mcp23017](../components/mcp23017), [board_v8](../main/board/board_v8.hpp) | Implemented | — |
| Buttons (4 × tactile, click + long-press) | [button_handler.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/drivers/buttons/button_handler.cpp) | [buttons](../components/buttons) | Implemented | — |
| SSD1680 EPDs (2.13" + 2.9", shared SPI bus, shadow-FB partial refresh) | [epd.cpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/drivers/epd/epd.cpp) (GxEPD2 library) | [epd_ssd1680](../components/epd_ssd1680) | Implemented (native driver) | — |
| BH1750 ambient-light sensor | BH1750 Arduino lib | [bh1750](../components/bh1750) | Implemented (sensor read); no auto-off consumer | `btclock_v3_fci-7ma` |
| PCA9685 frontlight driver | PCA9685 Arduino lib | [pca9685](../components/pca9685) | Implemented (init + static duty); no effects | `btclock_v3_fci-7ma` |
| Inverse-buttons pref (`InverseButtons`) | button_handler.cpp | — | Missing | `btclock_v3_fci-jwz` |

## Persistence (NVS / settings)

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| NVS wrapper | Arduino `Preferences` | [prefs](../components/prefs) | Implemented | — |
| `PrefKeys::*` catalog (~80 keys, see [pref_keys.hpp](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/src/lib/system/pref_keys.hpp)) | pref_keys.hpp | [components/settings/include/settings/pref_keys.hpp](../components/settings/include/settings/pref_keys.hpp) — 76 keys under namespace `settings`, 15-char limit statically enforced | Implemented | — |
| Settings schema + PATCH validation | settings.cpp `onApiSettingsPatch` | [settings_api.cpp](../components/settings/settings_api.cpp) — typed field table, boot-only classification, range + enum validation | Implemented | — |
| Screen-order NVS + catalog merge | config.cpp `rebuildScreenMappings`, `DEFAULT_SCREEN_ORDER` | — | Missing | `btclock_v3_fci-jek` |
| Factory-reset / "erase settings" flow | WiFiManager reset path | — | Missing | `btclock_v3_fci-sjy` |

## Build / board variants

| Feature | Old firmware | BTClock v4 | Status | Tracking |
|---|---|---|---|---|
| Rev A (Lolin S3 mini) | [platformio.ini](https://git.btclock.dev/btclock/btclock_v3/src/commit/eac3a28/platformio.ini) `env:lolin_s3_mini*` | [board_rev_a.hpp](../main/board/board_rev_a.hpp), `-DBTCLOCK_BOARD=REV_A` | Implemented | — |
| Rev B | platformio.ini `env:btclock_rev_b*` | [board_rev_b.hpp](../main/board/board_rev_b.hpp) (default) | Implemented | — |
| V8 (16 MB, dual MCP) | platformio.ini `env:btclock_v8*` | [board_v8.hpp](../main/board/board_v8.hpp) | Implemented | — |
| 2.13" EPD (GDEY0213B74) | `-D VERSION_EPD_2_13` | `-DBTCLOCK_PANEL=2_13` (default — not a constraint; any board × any panel configures) | Implemented | — |
| 2.9" EPD (GDEY029T94) | `-D VERSION_EPD_2_9` | `-DBTCLOCK_PANEL=2_9` (validated on Rev A; other boards configure but un-flashed) | Implemented | — |
| 7.5" EPD (GDEY075T7, UC8179) | n/a | `-DBTCLOCK_PANEL=7_5` (scaffold only — un-flashed; intended for Rev B) | Stubbed | — |
| CI matrix (4 env builds) | `.gitea/workflows` | [sdkconfig.defaults.*](../sdkconfig.defaults) + BTCLOCK_BOARD | Partial: no GitHub Actions yet | `btclock_v3_fci-x4k` |

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
