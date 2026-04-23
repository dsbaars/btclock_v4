# BTClock IDF C++ PoC — Feature Parity Matrix

Tracks every user-visible feature of the production Arduino/PlatformIO firmware
(`src/`) against the in-progress ESP-IDF C++ port (`idf_cpp_proto/`). Use this
as the single checklist when deciding what to port next.

_Last updated: **2026-04-23**. This doc is hand-maintained — keep in sync when
features land._

**Status tokens**

- Implemented — feature is at parity (or intentionally scoped equivalent).
- Partial — functional but missing sub-behaviors; notes list the gap.
- Stubbed — endpoint/surface exists but does nothing useful (returns 501 or a
  placeholder value).
- Missing — no code exists in the IDF port yet.
- N/A — intentionally dropped (note why).

**Branch state (2026-04-23, post merge-train):**

`feature/idf-cpp-poc` is now the baseline with seven merged worktrees:
WASM (a50ba51c), LittleFS (a1d216a4), Frontlight (a437993e), Control API
(aa1d0cd8), Nostr + blockfee2 (a2074e28), TLS gate + 7 mining-pool
DataSources (a7d7097f), Time/Halving/Supply/MarketCap screens (af32422c).
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

Post merge-train: 8 of 11 rotation screens render (block, clock, halving,
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

Enum values from [`src/lib/system/shared.hpp`](../../src/lib/system/shared.hpp)
lines 45-67; rotation catalog in
[`src/lib/system/config.cpp:45-68`](../../src/lib/system/config.cpp). IDF PoC
renderers under [`main/screens/`](../main/screens).

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| Block height (`SCREEN_BLOCK_HEIGHT`) | [screen_handler.cpp](../../src/lib/ui/screen_handler.cpp) | [block_height.cpp](../main/screens/block_height.cpp) | Implemented | — |
| Sats per currency (`SCREEN_SATS_PER_CURRENCY`, Moscow-time) | screen_handler.cpp | [moscow_time.cpp](../main/screens/moscow_time.cpp) | Implemented (USD + EUR/GBP/JPY) | — |
| BTC ticker (`SCREEN_BTC_TICKER`) | screen_handler.cpp | [btc_price.cpp](../main/screens/btc_price.cpp) | Implemented (multi-currency) | — |
| Time / clock (`SCREEN_TIME`) | screen_handler.cpp | [clock.cpp](../main/screens/clock.cpp) | Implemented (blocks-mode) | `btclock_v3_fci-lx0.13` |
| Halving countdown (`SCREEN_HALVING_COUNTDOWN`) | screen_handler.cpp | [halving.cpp](../main/screens/halving.cpp) | Partial: blocks-mode only; years/days/hours/mins mode deferred | `btclock_v3_fci-lx0.14` |
| Block fee rate (`SCREEN_BLOCK_FEE_RATE`) | screen_handler.cpp | [fee_rate.cpp](../main/screens/fee_rate.cpp) | Partial: integer sats/vB only; no "sat/vB" unit glyph; blockfee2 decimal variant pending | `btclock_v3_fci-wbr`, `btclock_v3_fci-znf` |
| Market cap (`SCREEN_MARKET_CAP`) | screen_handler.cpp | [market_cap.cpp](../main/screens/market_cap.cpp) | Partial: integer digits; M/B/T suffix "bigChars" mode deferred | `btclock_v3_fci-lx0.16` |
| Bitcoin supply (`SCREEN_BITCOIN_SUPPLY`) | screen_handler.cpp | [bitcoin_supply.cpp](../main/screens/bitcoin_supply.cpp) | Partial: integer BTC (capped 20,999,999); bigChars + percentage modes deferred | `btclock_v3_fci-lx0.15` |
| Mining pool hashrate / earnings (`SCREEN_MINING_POOL_STATS_*`) | screen_handler.cpp | — | Missing (7 data sources landed; renderer pending) | `btclock_v3_fci-lcw.1` |
| Bitaxe hashrate / best difficulty (`SCREEN_BITAXE_*`) | screen_handler.cpp | — | Missing | `btclock_v3_fci-lcw.2` |
| Runtime-pushed custom / text / countdown (`SCREEN_CUSTOM`, `SCREEN_COUNTDOWN`) | screen_handler.cpp, actions.cpp `/api/show/text` | Route stubbed 501 | Stubbed | `btclock_v3_fci-odc` |
| User-configurable rotation order | [config.cpp](../../src/lib/system/config.cpp) `rebuildScreenMappings`, default `DEFAULT_SCREEN_ORDER` | — | Missing | `btclock_v3_fci-jek` |
| Steal focus on new block (auto-flip to block screen) | `DEFAULT_STEAL_FOCUS`, ScreenHandler | Partial: `ConsumeNewBlock` drives LED flash only | Partial | `btclock_v3_fci-wn6` |
| Full periodic refresh (EPD ghost clear) | `DEFAULT_MINUTES_FULL_REFRESH` | — | Missing | `btclock_v3_fci-6fi` |
| Screen visibility toggles | settings.cpp per-screen flags | — | Missing | `btclock_v3_fci-jwz` |

## Data sources

Old-firmware data-source modules live under
[`src/lib/data_sources/`](../../src/lib/data_sources). IDF PoC uses a
`DataHub` + `DataSource` abstraction
([`components/data_core`](../components/data_core)). Multiple concrete
sources exist; not all are wired into `main.cpp` yet.

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| DataHub / registry abstraction | [live_service.hpp](../../src/lib/data_sources/live_service.hpp) | [hub.hpp](../components/data_core/include/data_core/hub.hpp) | Implemented | — |
| BTClock WS v2 (`BTCLOCK_SOURCE`) | [v2_notify.cpp](../../src/lib/data_sources/v2_notify.cpp) | [btclock_data.cpp](../components/btclock_data/btclock_data.cpp) | Partial: block + price + blockfee + blockfee2 subs; mempool sub-channels still missing | `btclock_v3_fci-lcw` |
| mempool.space third-party (`THIRD_PARTY_SOURCE`) | [block_notify.cpp](../../src/lib/data_sources/block_notify.cpp), [price_notify.cpp](../../src/lib/data_sources/price_notify.cpp) | — | Missing | `btclock_v3_fci-lcw` |
| Nostr source + Zap notifier (`NOSTR_SOURCE`) | [nostr_notify.cpp](../../src/lib/data_sources/nostr_notify.cpp) | [components/nostr/](../components/nostr) — relay client, REQ/EVENT sub, zap listener (kind 9735), NostrDataSource (kind 30078) | Partial: skeleton complete; event→snapshot dispatch in-flight; main.cpp NVS-gated wiring pending | `btclock_v3_fci-0wm` |
| Custom WS endpoint (`CUSTOM_SOURCE`) | config.cpp `ceEndpoint` | — | Missing | `btclock_v3_fci-lcw` |
| Kraken price fallback | price_notify.cpp | — | Missing | `btclock_v3_fci-lcw` |
| Multi-currency active set (`actCurrencies`) | config.cpp `getActiveCurrencies` | Hardcoded `{USD,EUR,GBP,JPY}` in main.cpp | Partial: no NVS-backed pref | `btclock_v3_fci-lx0.11` (sats symbol), `btclock_v3_fci-jwz` (settings schema) |
| Bitaxe HTTP poll | [bitaxe_fetch.cpp](../../src/lib/data_sources/bitaxe_fetch.cpp) | — | Missing | `btclock_v3_fci-lcw` |
| Mining pool stats (7 pools behind shared base + TLS gate) | [mining_pool/*](../../src/lib/data_sources/mining_pool) | [mining_pool_common/](../components/mining_pool_common) + per-pool components (braiins / ocean / noderunners / satoshi_radio / public_pool / ckpool+EU / gobrrr) | Partial: all 7 DataSources ported and host-tested; no main.cpp wiring; pool-selection orchestrator + NVS pref pending | `btclock_v3_fci-093`, `-zxk`, `-zs3`, `-qmh`, `-g7h`, `-dhs`, `-9c1` |
| TLS gate (single-flight mbedtls handshake) | [tls_gate.hpp](../../src/lib/system/tls_gate.hpp) | [components/tls_gate/](../components/tls_gate) | Implemented; wrapped around each mining-pool HTTPS handshake | `btclock_v3_fci-3kt` |
| Emscripten/WASM preview build (IDF-port helpers) | [lib/btclock/data_handler.cpp](../../lib/btclock/data_handler.cpp) (`__EMSCRIPTEN__`) | [tools/wasm/](../tools/wasm) — binding compiles `screens/common.cpp` + `screen_math.cpp` | Partial: 7 `parse*` bindings live (all on-device screens); pixel-accurate framebuffer + font rasterisation (Phase 2) in-flight | `btclock_v3_fci-90q` |

## HTTP / Control API

Old firmware registers all routes via `server.on(...)` under
[`src/lib/net/webserver/`](../../src/lib/net/webserver). IDF PoC control
routes live in
[`components/webserver/control_server.cpp`](../components/webserver/control_server.cpp)
on `feature/idf-cpp-poc`. Stub rows return HTTP 501 with a tracking
token pointing at the follow-up issue.

| Endpoint | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| `GET /api/status` | [status.cpp](../../src/lib/net/webserver/status.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) | Partial: `data[]`/`leds[]`/`dnd` are placeholder shape only | `btclock_v3_fci-dm9` |
| `GET /api/system_status` | status.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (real FS fields via `btclock::GetLittleFsUsage`) | — |
| `GET /api/settings` | [settings.cpp](../../src/lib/net/webserver/settings.cpp) | Stubbed 501 | Stubbed | `btclock_v3_fci-jwz` |
| `PATCH /api/settings` | settings.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-jwz` |
| `POST /api/full_refresh` | [actions.cpp](../../src/lib/net/webserver/actions.cpp) | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (event loop consumes `kFullRefresh` → `ScreenManager::MarkDirty`) | — |
| `POST /api/identify` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Partial: queued, LED effect not wired | `btclock_v3_fci-fxh` |
| `POST /api/restart` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/screen` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/currency` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/show/text` | actions.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-odc` |
| `POST /api/show/custom` | (derived: custom screen) | Stubbed 501 | Stubbed | `btclock_v3_fci-odc` |
| `POST /api/screen/next` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/screen/previous` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/action/pause` | actions.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-xgv` |
| `POST /api/action/timer_restart` | actions.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-xgv` |
| `POST /api/stop_datasources` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/restart_datasources` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented | — |
| `POST /api/wifi_set_tx_power` | actions.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (range-validated quarter-dBm via `esp_wifi_set_max_tx_power`) | — |
| `GET /api/dnd/status` | [dnd.cpp](../../src/lib/net/webserver/dnd.cpp) | Stubbed 501 | Stubbed | `btclock_v3_fci-egh` |
| `POST /api/dnd/enable` | dnd.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-egh` |
| `POST /api/dnd/disable` | dnd.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-egh` |
| `GET /api/lights` | [lights.cpp](../../src/lib/net/webserver/lights.cpp) | Stubbed 501 | Stubbed | `btclock_v3_fci-fxh` |
| `POST /api/lights/color` | lights.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-fxh` |
| `POST /api/lights/off` | lights.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-fxh` |
| `POST /api/frontlight/on` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (FrontlightIface → `FrontlightController::On`; 503 on boards without frontlight) | — |
| `POST /api/frontlight/off` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (FrontlightIface → `FrontlightController::Off`) | — |
| `POST /api/frontlight/flash` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (block-flash pulse) | — |
| `GET /api/frontlight/status` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (JSON: enabled/duty/target/threshold/auto-off) | — |
| `POST /api/frontlight/brightness` | lights.cpp | [control_server.cpp](../components/webserver/control_server.cpp) | Implemented (validated duty → fade) | — |
| `POST /upload/firmware` | [ota_routes.cpp](../../src/lib/net/webserver/ota_routes.cpp) | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| `POST /upload/webui` | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| `POST /api/firmware/auto_update` | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| SSE event stream (`/events`) | [webserver.cpp](../../src/lib/net/webserver/webserver.cpp) | — | Missing | `btclock_v3_fci-equ` |
| Static WebUI file server (`/`) | webserver.cpp `serveStatic` | — (portal only; no main UI) | Missing | `btclock_v3_fci-equ` |
| HTTP Basic auth gate | webserver.cpp `requireHttpAuth` | — | Missing | `btclock_v3_fci-equ` |
| Screen-order REST API | config.cpp + settings.cpp (3xh/36t closed) | — | Missing | `btclock_v3_fci-jwz`, `btclock_v3_fci-jek` |

## Provisioning / WiFi

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| WiFiManager captive portal (first-boot AP) | WiFiManager library in main.cpp | [provisioning_server.cpp](../components/webserver/provisioning_server.cpp) + [boot_ui](../main/boot_ui.cpp) | Implemented | — |
| WPA2 AP with random password | (N/A — open AP historically) | main.cpp `MakeOrLoadApPassword` | Implemented (superset) | — |
| On-panel SSID + password + QR display | — | [provisioning_ui.cpp](../main/provisioning_ui.cpp) | Implemented (superset) | — |
| SSID scan API | (bundled in WiFiManager) | `GET /api/scan` (background scan on branch aa1d0cd8) | Implemented | — |
| `GET /api/version` (hw/fw/idf) | status.cpp (nested) | provisioning_server.cpp | Implemented | — |
| Captive-portal DNS hijack | WiFiManager | [dns_hijack.cpp](../components/webserver/dns_hijack.cpp) | Implemented | — |
| mDNS advertisement (`http._tcp`) | webserver.cpp `MDNS.begin(...)` | — | Missing | `btclock_v3_fci-equ` |
| Auto-reconnect + 10-minute reboot on WiFi loss | main.cpp `checkWiFiConnection` | [wifi_guard.cpp](../main/app/wifi_guard.cpp) (block-until-connected only) | Partial: no long-outage reboot timer | `btclock_v3_fci-79f` |

## LED + light subsystems

Old firmware is in
[`src/lib/drivers/leds/led_handler.cpp`](../../src/lib/drivers/leds/led_handler.cpp)
(3-in-1: NeoPixel state, DND, frontlight/PCA9685 fade). IDF PoC's
[`app/led_controller.cpp`](../main/app/led_controller.cpp) is a minimal 3-event
state machine (boot / idle / block-flash).

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| NeoPixel FreeRTOS task + queue | led_handler.cpp | led_controller.cpp | Implemented (3 events vs ~25) | `btclock_v3_fci-fxh` |
| Boot / idle / block-flash effects | led_handler.cpp `LED_FLASH_*` | led_controller.cpp | Implemented (subset) | — |
| Full effect catalog (heartbeat, timer, WiFi states, progress bars, identify, zap, data errors, ~25 effects) | led_handler.cpp `LED_*` constants | — | Missing | `btclock_v3_fci-fxh` |
| LED brightness + color + flash-on-update prefs (`DEFAULT_LED_BRIGHTNESS`, `BlockFlashColor`) | led_handler.cpp + defaults.hpp | — (hardcoded orange) | Missing | `btclock_v3_fci-fxh`, `btclock_v3_fci-jwz` |
| Frontlight PCA9685 channels init | led_handler.cpp `#ifdef HAS_FRONTLIGHT` | [frontlight_controller.cpp](../main/app/frontlight_controller.cpp) drives fade on boot | Implemented | `btclock_v3_fci-7ma` |
| Frontlight fade + flash-on-block + flash-on-zap | led_handler.cpp `frontlightFadeIn/OutAll` | frontlight_controller.cpp — fade + block-flash wired from `ConsumeNewBlock`; zap-flash event exposed but no Nostr source to trigger it yet | Partial: zap-flash awaits Nostr source (`btclock_v3_fci-lcw`) | `btclock_v3_fci-7ma` |
| Ambient-light auto-off (BH1750) | main.cpp `handleFrontlight` | main.cpp heartbeat feeds lux to `FrontlightController::OnAmbientLux`; threshold runtime-configurable, NVS persistence deferred | Partial: no NVS persistence of threshold/enable | `btclock_v3_fci-7ma`, `btclock_v3_fci-jwz` |
| `DisableLeds` / `FlDisable` NVS toggles | defaults.hpp | — | Missing | `btclock_v3_fci-fxh`, `btclock_v3_fci-jwz` |

## DND / scheduling

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| DND enabled flag (force-off LEDs) | led_handler.cpp `setDNDEnabled` | — | Missing | `btclock_v3_fci-egh` |
| Time-based DND window (start/end HH:MM) | led_handler.cpp `setDNDTimeRange`, [lib/dnd_window.cpp](../../lib/dnd_window.cpp) | — | Missing | `btclock_v3_fci-egh` |
| DND status surfaced in `/api/status` | status.cpp nested `dnd` object | control_server.cpp emits `{enabled:false,active:false}` placeholder (branch) | Stubbed | `btclock_v3_fci-egh` |
| Screen-rotation timer active / pause | `DEFAULT_TIMER_ACTIVE`, actions.cpp | screen_manager auto-rotates unconditionally | Missing | `btclock_v3_fci-xgv` |

## OTA / updates

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| Web-UI firmware upload (`U_FLASH`) | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| Web-UI LittleFS upload (`U_SPIFFS`) | ota_routes.cpp | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| Auto-update check (GitHub releases) | ota_routes.cpp `onAutoUpdateFirmware`, `gitReleaseUrl` | Stubbed 501 | Stubbed | `btclock_v3_fci-5b2` |
| ArduinoOTA push (PlatformIO → device) | [ota.cpp](../../src/lib/net/ota/ota.cpp) | — | Missing | `btclock_v3_fci-5b2` |

## Peripherals

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| MCP23017 expander(s), incl. V8 dual-chip | [shared.hpp](../../src/lib/system/shared.hpp) `mcp1/mcp2`, epd.cpp | [mcp23017](../components/mcp23017), [board_v8](../main/board/board_v8.hpp) | Implemented | — |
| Buttons (4 × tactile, click + long-press) | [button_handler.cpp](../../src/lib/drivers/buttons/button_handler.cpp) | [buttons](../components/buttons) | Implemented | — |
| SSD1680 EPDs (2.13" + 2.9", shared SPI bus, shadow-FB partial refresh) | [epd.cpp](../../src/lib/drivers/epd/epd.cpp) (GxEPD2 library) | [epd_ssd1680](../components/epd_ssd1680) | Implemented (native driver) | — |
| BH1750 ambient-light sensor | BH1750 Arduino lib | [bh1750](../components/bh1750) | Implemented (sensor read); no auto-off consumer | `btclock_v3_fci-7ma` |
| PCA9685 frontlight driver | PCA9685 Arduino lib | [pca9685](../components/pca9685) | Implemented (init + static duty); no effects | `btclock_v3_fci-7ma` |
| Inverse-buttons pref (`InverseButtons`) | button_handler.cpp | — | Missing | `btclock_v3_fci-jwz` |

## Persistence (NVS / settings)

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| NVS wrapper | Arduino `Preferences` | [prefs](../components/prefs) | Implemented | — |
| `PrefKeys::*` catalog (~80 keys, see [pref_keys.hpp](../../src/lib/system/pref_keys.hpp)) | pref_keys.hpp | Only `net.ssid`, `net.pw`, `net.app` (AP pw) | Missing (~77 of ~80 keys) | `btclock_v3_fci-jwz` |
| Settings schema + PATCH validation | settings.cpp `onApiSettingsPatch` | — | Missing | `btclock_v3_fci-jwz` |
| Screen-order NVS + catalog merge | config.cpp `rebuildScreenMappings`, `DEFAULT_SCREEN_ORDER` | — | Missing | `btclock_v3_fci-jek` |
| Factory-reset / "erase settings" flow | WiFiManager reset path | — | Missing | `btclock_v3_fci-sjy` |

## Build / board variants

| Feature | Old firmware | IDF PoC | Status | Tracking |
|---|---|---|---|---|
| Rev A (Lolin S3 mini) | [platformio.ini](../../platformio.ini) `env:lolin_s3_mini*` | [board_rev_a.hpp](../main/board/board_rev_a.hpp), `-DPOC_BOARD=REV_A` | Implemented | — |
| Rev B | platformio.ini `env:btclock_rev_b*` | [board_rev_b.hpp](../main/board/board_rev_b.hpp) (default) | Implemented | — |
| V8 (16 MB, dual MCP) | platformio.ini `env:btclock_v8*` | [board_v8.hpp](../main/board/board_v8.hpp) | Implemented | — |
| 2.13" EPD | `-D VERSION_EPD_2_13` | `PanelKind::k2_13` in main.cpp | Implemented | — |
| 2.9" EPD | `-D VERSION_EPD_2_9` | epd_ssd1680 supports it; wired for 2.13 in main.cpp | Partial: wiring hardcoded | `btclock_v3_fci-znm` |
| CI matrix (4 env builds) | `.gitea/workflows` | [sdkconfig.defaults.*](../sdkconfig.defaults) + POC_BOARD | Partial: no GitHub Actions yet | `btclock_v3_fci-x4k` |

---

## How to use this doc

Treat this as a ground-truth checklist, not a history log. When you land a
feature in the IDF port, flip its row here (Missing/Stubbed → Partial →
Implemented) in the same PR, and drop the tracking link when the beads issue
closes. Beads issues remain the source of truth for in-flight work — this
matrix just collects them in one scannable place. If you discover a feature
gap the table misses, add a row; if you're unsure whether it's a distinct
feature, fold it into the nearest parent row as a sub-bullet rather than
inventing a new one.
