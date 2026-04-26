# BTClock settings reference

All settings live under the NVS namespace `settings` and are exposed via
`GET /api/settings` and `PATCH /api/settings`. Changes via PATCH persist
to NVS and take effect without reboot unless the "Notes" column says
otherwise. A PATCH that writes a reboot-only field still persists the
value immediately; the response is `{"rebootRequired": true}` so the
WebUI can prompt the user.

Source of truth for the list is
`components/settings/include/settings/schema.hpp` (67 scalar fields),
plus a handful of special-case keys (`dnd`, `screens`, `actCurrencies`,
`timePerScreen`, `invertedColor`, `txPower`) handled directly in
`components/settings/settings_api.cpp::ApplyPatch()`.

Defaults in the table below come from `components/settings/include/settings/schema.hpp`'s
`FieldSpec::default_*` fields, ported from the old firmware's
`btclock_v3_fci/src/lib/system/defaults.hpp`. A fresh install (NVS wiped,
or a key never PATCHed) surfaces these values.

## JSON type mapping

| Schema type | JSON shape | NVS storage |
|---|---|---|
| `bool` | `true` / `false` | NVS u8 (0/1) |
| `uint` | JSON number (>= 0) | NVS u32 |
| `int` | JSON number | NVS i32 |
| `uchar` | JSON number (0..255) | NVS u8 |
| `string` | JSON string | NVS string |
| hex RGB | JSON number (24-bit integer) | NVS u32, e.g. `0xFF8800` is sent as `16746496` |
| time-of-day | split into `dnd.startHour` / `dnd.startMinute` etc. | four NVS u32 keys |
| CSV | emitted as a JSON array, persisted as a comma-joined string | NVS string |

Unknown top-level keys in a PATCH body are silently ignored (matches
the old firmware). Known keys with the wrong type return 400 with a
structured `{"error": "<key>:bad_type"}` body. Range-constrained keys
return 400 with `{"error": "range:<key>"}`.

## Grouping rationale

Settings are grouped by the subsystem that owns them rather than by
schema type, so the reader can find "where does the Bitaxe host go?"
without scanning all booleans. Each group ends with any special-case
derived keys the WebUI uses to render that section.

---

## Display

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `fontName` | string (enum) | `"antonio"` | Font family used by label / digit / small-chars / unit screen roles. | Values come from `availableFonts` in GET (`antonio`, `oswald`, `inter`, `sourceSerif`, `merriweather`, `bitter`, `atkinson`). Rebound live via the `on_font_changed` hook, which marks the screen dirty for a repaint. PATCH validates against the catalogue. Devices upgrading from a build that stored the retired `"dejavu"` value fall back to `"antonio"` at boot via the validation walk. |
| `invertedColor` | bool | `true` | White-on-black when `true`, black-on-white when `false`. | PATCH side-writes `fgColor` / `bgColor` to keep EPD polarity consistent. Applied live: `EpdSetGlobalInverted` + `ScreenManager::MarkDirty`. |
| `fgColor` | uint (hex RGB) | `0xFFFF` | EPD foreground colour. | Auto-updated when `invertedColor` is PATCHed; schema does not expose it as a PATCH key, but GET emits it. |
| `bgColor` | uint (hex RGB) | `0x0000` | EPD background colour. | Same auto-update rule as `fgColor`. |
| `fullRefreshMin` | uint | `60` | Minutes between mandatory full-refresh cycles on the EPDs. | Range 0..1440. Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `refrScrnChange` | bool | `false` | Force a full EPD refresh on every screen change. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `verticalDesc` | bool | `true` | Rotate label panels 90° CCW so text reads along the panel's long axis. | Honored by `PaintSlotIntoFb` for `kLabel` / `kLabelSplit` slots (ports v3 `splitText`'s verticalDesc branch). Applied live via `ScreenManager::LoadScreenRenderPrefs`. |
| `mcapBigChar` | bool | `true` | Use big-character layout for market-cap and supply screens. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `supplyPercent` | bool | `false` | Render bitcoin supply as a percentage of 21M rather than absolute BTC. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `useBlkCountdown` | bool | `true` | Halving screen: count in blocks remaining, not days. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `useMscwTime` | bool | `true` | Show the sats-per-dollar ("Moscow Time") flip layout. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `useSatsSymbol` | bool | `false` | Use the sats-glyph font for price suffixes. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `blockFeeDec` | bool | `true` | Show decimal sats/vB on the fee-rate screen when the data source reports a precise value. | Honored in `ScreenManager` rendering path. |
| `suffixPrice` | bool | `false` | Use k/M suffixes on the price screen. | Honored by `ScreenManager::ReadRenderPrefs` and threaded through `RenderBtcPriceScreen` / `BuildBtcPrice`. |
| `suffixShareDot` | bool | `false` | When `true`, the decimal-point dot shares the digit panel before it instead of taking its own panel — frees one panel for an extra digit before or after the separator. When `false` (default), the dot occupies its own panel. | Honored by `LayoutBtcPriceSuffixStrings` via `ScreenManager::ReadRenderPrefs`. Folds the dot into the preceding digit cell so K/M-suffix layouts get one more digit of width. |
| `mowMode` | bool | `false` | Million-Of-Watoshis style price formatting. | Honored by `ScreenManager::ReadRenderPrefs` and `LayoutBtcPriceSuffixStrings`. |
| `hideLeadZero` | bool | `false` | Clock screen: drop the leading zero on single-digit hours ("07:00" → "7:00"). Minute leading zero is always preserved. | Honored by `ComputeClockLayout` via `ScreenManager::ReadRenderPrefs`. Applied live: `on_settings_patched` calls `ScreenManager::MarkDirty`. NVS key truncated to `hideLeadZero` (15-char cap; JSON key matches). |

Display-related special keys:

| Key | Type | Description |
|---|---|---|
| `timePerScreen` | uint (minutes) | PATCH-only alias. Writes `timerSeconds = timePerScreen * 60`. Sanity capped at 3600 s. |
| `timerSeconds` | uint (seconds) | GET-only (read from NVS key `timerSeconds`, default 1800). Auto-rotate period. |
| `screens` | array of `{id, name, enabled, order}` | Per-screen visibility + rotation order. PATCH accepts either visibility-only toggles or a full reorder. Partial reorders are rejected. `screen<id>Visible` NVS keys default to `true`. |
| `screenOrder` | string (CSV of ids) | Persisted rotation order. Written indirectly by PATCHing `screens[].order`. |
| `currentScreen` | uint | Last-shown screen id. Used for restore-on-boot; not PATCH-settable. |

---

## Network

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `hostnamePrefix` | string | `"btclock"` | Hostname prefix used for mDNS + DHCP. The MAC suffix is appended. | Reboot required. |
| `mdnsEnabled` | bool | `true` | Advertise `_http._tcp` and `_btclock._tcp` over mDNS. | Reboot required. Honored in `main/app/mdns_service.cpp`. |
| `wifiRebootMin` | uint | `10` | Minutes of continuous STA disconnect before a soft reboot. `0` disables. | Range 0..120. Live — the wifi_guard tick re-reads on every fire. |
| `wpTimeout` | uint | `900` (15 min) | WiFiManager captive-portal timeout (seconds). | Range 0..3600. Reboot required. After this many seconds in AP-provisioning mode the device reboots so the next boot retries STA — gated on `wifiConfigured` being true (set once the user submits creds), so a never-provisioned device sits in the portal indefinitely. |
| `txPower` | int | device default | WiFi TX power in quarter-dBm. `-1..78` valid; `80` is a sentinel that clears the override. | Live. Applied via `esp_wifi_set_max_tx_power`. Re-applied at boot in `init_network.cpp` between `esp_wifi_start` and `Connect`, mirroring the same `IsValidWifiTxPower` range gate. Queried back on `/api/status` via `esp_wifi_get_max_tx_power`. |
| `wifiConfigured` | bool | `false` | Set by the provisioning flow once STA credentials are captured. | Not PATCH-settable in practice — internal flag. |

---

## Data sources

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `dataSource` | uchar (enum) | `0` | Selects the BTC-price / block / fee data source. `0`=BTClock WS, `1`=third-party custom endpoint, `2`=Nostr. | Range 0..3. Reboot required. |
| `mempoolInstance` | string | `"mempool.space"` | Base URL of the mempool.space-compatible instance. | Reboot required. |
| `mempoolSecure` | bool | `true` | Use HTTPS/WSS when talking to `mempoolInstance`. | Reboot required. |
| `ceEndpoint` | string | `"ws-staging.btclock.dev"` | Custom-endpoint host when `dataSource=1`. | Reboot required. |
| `ceDisableSSL` | bool | `false` | Disable TLS for the custom endpoint. | Reboot required. |
| `minSecPriceUpd` | uint | `30` | Minimum seconds between price updates applied to the EPD. | Range 1..3600. Live. Throttles the EPD price-write path (kBtcPrice / kMoscowTime / kMarketCap) inside `ScreenManager::ShouldRender` so a chatty WS price stream can't burn the e-paper. Nav events and force-fulls always paint. `0` disables. |
| `actCurrencies` | CSV string | `"USD,EUR,JPY"` | Comma-joined ISO-4217 codes that appear in the ticker / moscow-time rotation. | GET emits a filtered array; PATCH accepts an array. Codes outside `availableCurrencies` are silently dropped. |

---

## Light & LEDs

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `ledBrightness` | uint (0..255) | `128` | NeoPixel master brightness. | Range 0..255. Stored under NVS namespace `led/brightness` by the LED controller (separate namespace). The `settings`-namespace value mirrors the WebUI PATCH write; consumers read the `led` namespace at boot. |
| `blockFlashColor` | uint (hex RGB) | `0xE04300` | RGB colour pulsed on LEDs when a new block arrives. | Range 0..0xFFFFFF. See `led` namespace note above. |
| `disableLeds` | bool | `false` | Master mute flag for the NeoPixel strip. | Honored by `led_controller.cpp`. |
| `ledFlashOnUpd` | bool | `false` | Flash LEDs on new-block / price-update events. | Honored by `led_controller.cpp`. |
| `ledFlashOnZap` | bool | `true` | Flash LEDs on incoming Nostr zap receipt. | Live — zap listener rechecks per receipt. |
| `ledTestOnPower` | bool | `true` | Run the rainbow LED self-test at boot. | Honored by `InitBootLeds`: when false, posts `kSetIdle` instead of the rainbow `kSetBoot`. Reboot required. |
| `flAlwaysOn` | bool | `true` | Frontlight always on, overriding ambient-off. | Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `flDisable` | bool | `false` | Master mute for the frontlight. | Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `flEffectDelay` | uint | `15` | Fade-effect step delay (ms). | Range 0..1000. Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `flFlashOnUpd` | bool | `true` | Flash frontlight on update events. | Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `flFlashOnZap` | bool | `true` | Flash frontlight on Nostr zap receipt. | Live — zap listener rechecks per receipt. |
| `flMaxBrightness` | uint | `2048` (`kDefaultMaxDuty`) | Frontlight PWM duty at 100 percent. | Range 0..65535. Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `flOffWhenDark` | bool | `true` | Turn frontlight off when ambient lux is very low. | Rev B only. Hysteresis: enters dark at 1.0 lux, exits at 2.0 lux. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
| `luxLightToggle` | uint | `128` (`kDefaultLuxThreshold`) | Ambient-lux threshold that flips the frontlight between "room bright" and "room dim". | Range 0..65535. Rev B only. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. `0` disables the auto-off feature (matches v3). |
| `inverseButtons` | bool | `false` | Swap the button-1/button-4 polarity. | Honored by `ButtonReader::SetInverted` — physical pin `i` is delivered as logical `ButtonId(N-1-i)`. Read once at boot from `sources.cpp`; reboot required. |

---

## Do Not Disturb

GET emits a nested `dnd` object; PATCH takes the same shape.

| Nested key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `dnd.enabled` | bool | `false` | Manual "force on now" flag. | Live. |
| `dnd.dndTimeEnabled` | bool | `false` | Enable scheduled DND. | Live, mirrored into the DND singleton by `on_dnd_changed`. |
| `dnd.startHour` | uint (0..23) | `22` | Schedule start hour. | Live. |
| `dnd.startMinute` | uint (0..59) | `0` | Schedule start minute. | Live. |
| `dnd.endHour` | uint (0..23) | `7` | Schedule end hour. | Live. |
| `dnd.endMinute` | uint (0..59) | `0` | Schedule end minute. | Live. All four hour/minute values must be present in the same PATCH or the block is skipped. |

NVS keys: `dndEnabled`, `dndTimeEnabled`, `dndStartHour`, `dndStartMin`,
`dndEndHour`, `dndEndMin`. Each is also listed individually in the
top-level GET response (so a GET returns both the flat keys and the
nested `dnd` block).

---

## Time zone

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `tzString` | string (IANA) | `"Europe/Amsterdam"` | IANA zone name (e.g. `"Europe/Amsterdam"`). | Live: PATCH triggers `timezone::SetTimezoneByName` which calls `setenv("TZ", ...) + tzset()`. |

Both `gmtOffset` and the `tzOffset` (minutes) PATCH alias were removed on 2026-04-24 (bd `btclock_v4-9rx`). v4 drives the clock from POSIX TZ strings via `setenv("TZ", ...) + tzset()`, and the old offset pref was never read back. Legacy clients that still send `gmtOffset` / `tzOffset` get a silent no-op (the rest of the PATCH body still applies).

---

## Mining pool

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `miningPoolStats` | bool | `false` | Master toggle — when `false` no pool data source runs. | Live at data-source restart. |
| `miningPoolName` | string (enum) | `"noderunners"` | Pool backend to use. | Values come from `availablePools` in GET: `ocean`, `noderunners`, `satoshi_radio`, `braiins`, `public_pool`, `local_public_pool`, `gobrrr_pool`, `ckpool`, `eu_ckpool`, `nerdminers_org`, `nerdminer_io`, `foundry_usa`, `viabtc`. PATCH validates against the catalogue. v4 default flipped from v3's `"ocean"` so a fresh device works without per-user credentials (paired with `poolGlobalStats=true`). |
| `miningPoolUser` | string | `"38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy"` | Primary identifier for the active pool. Semantics are per-pool. **Public-info pools** (Ocean payout address, Braiins username, CKPool worker, NerdMiner BTC address): emitted plaintext in GET. **Secret-key pools** (`viabtc`, `foundry_usa`): the value is an API key — GET suppresses the raw string and emits `miningPoolUserSet` (bool) instead, same protocol as `httpAuthPass`. See `docs/WEBUI_MINING_POOL_FIELDS.md` for the per-pool labels the WebUI should render. |
| `poolWorker` | string | `""` | Optional secondary identifier scoped under `miningPoolUser`. Used by `foundry_usa` for the subaccount path segment; reserved for `braiins` / `ckpool` worker name (currently unused by their parsers). Always plaintext in GET. |
| `poolGlobalStats` | bool | `true` | Noderunners / Satoshi Radio: show the pool-wide aggregate rather than the user-specific view. | Honored by `mining_pool_selector.cpp`. v4 default flipped from v3's `false` so the new default `noderunners` pool renders without per-user credentials. |
| `localPoolHost` | string | `"umbrel.local:2019"` | Host:port for `local_public_pool`. | Reboot required. |
| `poolPollSec` | uint | `60` | HTTPS poll cadence for the pool data source (seconds). | Range 10..3600. Live — `PoolDataSource::poll_interval_ms()` re-reads NVS each tick so a PATCH lands on the next poll without reboot. Bounds leave room to throttle past the legacy 60 s default for pools that publish per-minute stats already, without overwhelming free public endpoints. |
| `poolLogosUrl` | string | `"https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main"` | Base URL for fetching mining-pool logos. | Consumed by `pool_logo_fetcher.cpp::LogosBaseUrl()` — the on-demand fetcher pulls per-pool PNGs and caches them on LittleFS, so the firmware no longer ships every logo bitmap in flash. Trailing slashes are trimmed so concatenation can't yield a `//`. |

---

## Bitaxe

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `bitaxeEnabled` | bool | `false` | Poll a Bitaxe miner for hashrate / best difficulty. | Live at data-source restart. |
| `bitaxeHostname` | string | `"bitaxe1"` | LAN hostname or IP of the Bitaxe. | Consumed by `components/bitaxe/src/bitaxe_source.cpp`. |
| `bitaxePollSec` | uint | `10` | LAN poll cadence (seconds). | Range 5..300. Live — `BitaxeSource::Run()` re-reads NVS each tick so a PATCH lands on the next poll without reboot. Lower bound keeps the AxeOS HTTP server from being hammered while still allowing fast updates during bring-up. |

---

## Nostr / zap

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `nostrRelay` | string | `"wss://relay.primal.net"` | Relay URL for the primary data feed when `dataSource=2`. Also reused by the zap listener. | Reboot required. PATCH validates the scheme: only `wss://` or `ws://` are accepted (or an empty string to disable). Bare hostnames (`relay.example.com`) and `https://` URLs return `400 {"error":"nostrRelay:bad_scheme"}` — the underlying `esp_websocket_client` parser would otherwise silently reject them at boot with `Error parse uri`. |
| `nostrPubKey` | string (64-hex) | `"642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288"` | Author pubkey the data-source listener follows. | Reboot required. Validated: empty or exactly 64 lowercase-hex chars. |
| `nostrZapNotify` | bool | `false` | Listen for NIP-57 zap receipts and pop the zap screen. | Live (zap listener rechecks). |
| `nostrZapPubkey` | string (64-hex) | `"b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422"` | Pubkey whose zaps trigger the zap screen. | Live. Validated: empty or 64-hex. |
| `scrnRestoreZap` | bool | `true` | After a zap-screen pop, restore the screen that was active before. | Live. |
| `stealFocus` | bool | `false` | When a new block arrives, jump the display to the block-height screen so the fresh height appears without waiting for rotation. | Live — `event_loop.cpp` reads NVS per new-block event so a PATCH lands without reboot. Overlay-aware: debug / custom / zap overlays block the steal via `BlockEventPolicy::ShouldSteal`. |

---

## HTTP auth / OTA

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `httpAuthEnabled` | bool | `false` | Require HTTP basic auth on control-API endpoints. | Reboot required. |
| `httpAuthUser` | string | `"btclock"` | HTTP basic-auth username. | Reboot required. Default lives in `auth_gate.cpp::kDefaultUser`. |
| `httpAuthPass` | string | `""` | HTTP basic-auth password. | Reboot required. GET suppresses the value and emits `httpAuthPassSet` (bool) instead. |
| `otaEnabled` | bool | `true` | Enable the OTA firmware-upload endpoints. | Reboot required. |
| `otaPass` | string | `""` | Password gating the OTA firmware upload. | Reboot required. GET suppresses the value and emits `otaPassSet` (bool) instead. |
| `gitReleaseUrl` | string | `"https://git.btclock.dev/api/v1/repos/btclock/btclock_v3/releases/latest"` | Base URL the auto-update endpoint pulls releases from. | Live — consumed by `OtaManager::Init` and `/api/firmware/auto_update`. |

---

## Diagnostics

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `enableDebugLog` | bool | `false` | Enable verbose debug logging. | Reboot required. Honored in `InitStorage` immediately after NVS comes up: when true, `esp_log_level_set("*", ESP_LOG_DEBUG)` raises every subsystem's log level for the rest of the boot. The pre-NVS banner lines stay at INFO. |
| `timerActive` | bool | `true` | Rotation timer running. | Not typically PATCHed (use `/api/action/pause`); the pref exists for boot restore. |
| `blockHeight` | int | `0` | Cached latest block height. | Internal; persisted so the block screen can paint before WS reconnect. |

---

## Uncategorised

The following schema keys don't slot neatly into the groups above, or
are primarily internal:

| Key | Type | Default | Notes |
|---|---|---|---|
| `currentScreen` | uint | `0` | Last-displayed screen id. Restored on boot. |
| `timerSeconds` | uint | `1800` | Auto-rotate period (seconds). Written indirectly via `timePerScreen`. |
| `timerActive` | bool | `true` | Rotation timer running flag. |
| `fgColor` / `bgColor` | uint | `0xFFFF` / `0x0000` | Side-written by `invertedColor` PATCH. |
| `wifiConfigured` | bool | `false` | Provisioning-flow internal. |
| `actCurrencies` | string (CSV) | `"USD,EUR,JPY"` | Persisted shape; GET/PATCH use array shape. |
| `screenOrder` | string (CSV) | `""` | Persisted shape; GET/PATCH use `screens[].order`. |

---

## Read-only fields (GET only — not PATCH-settable)

These appear in the `GET /api/settings` response but are not writable
via PATCH. They're derived from the running firmware and the device
context.

| Key | Type | Source | Description |
|---|---|---|---|
| `hostname` | string | Wifi STA | Resolved `<hostnamePrefix>-<mac>` hostname. |
| `ip` | string | Wifi STA | Current STA IP, empty in AP mode. |
| `hwRev` | string | compile-time | Human-readable hardware revision (e.g. `"Rev B"`). |
| `fsRev` | string | compile-time | LittleFS bundle version string. |
| `gitRev` | string | compile-time | Git SHA of the firmware. Omitted when unknown. |
| `gitTag` | string | compile-time | Git tag, when the build was tagged. Omitted when empty. |
| `lastBuildTime` | int | compile-time | Firmware build time as Unix seconds (UTC). Omitted when the compiler emitted an unparseable `__DATE__`. |
| `numScreens` | int | board config | Number of EPD panels on this board (3 on Rev A / Rev B, 7 on V8). Emitted identically by `GET /api/status` so the WebUI's custom-text `maxlength` agrees with both endpoints. Not related to the screen-rotation slot count. |
| `screens[]` | array of objects | `kScreenKinds` + NVS | Per-screen rotation catalogue, see the Display section. |
| `availableFonts[]` | string array | `kAvailableFonts` | Font ids the WebUI picker shows. |
| `availablePools[]` | string array | `AvailablePoolNames()` | Mining-pool ids the WebUI picker shows. |
| `availableCurrencies[]` | string array | `kAvailableCurrencies` | ISO codes the price feed can serve. |
| `hasFrontlight` | bool | board capability | Whether this board has a PCA9685 frontlight (Rev B only today). |
| `hasLightLevel` | bool | board capability | Whether a BH1750 ambient sensor is populated (Rev B only today). |
| `lightLevel` | number (lux) | BH1750 | Current lux reading. Only emitted when `hasLightLevel` is true. |
| `httpAuthPassSet` | bool | NVS presence | `true` iff `httpAuthPass` is non-empty. |
| `otaPassSet` | bool | NVS presence | `true` iff `otaPass` is non-empty. |
| `miningPoolUserSet` | bool | NVS presence | Emitted **only** when the active `miningPoolName` is a secret-key pool (`viabtc`, `foundry_usa`); `true` iff `miningPoolUser` is non-empty. For public-info pools the raw `miningPoolUser` rides plaintext and this companion is omitted. |
| `timerRunning` | bool | runtime | Rotation-timer live state. |
| `invertedColor` | bool | NVS | Device-dependent default of `true` when the key is absent. |

Two additional GET-only bodies are surfaced by sibling endpoints that
the WebUI calls alongside `/api/settings`:

- `GET /api/system/status` — returns `txPower` (queried live via
  `esp_wifi_get_max_tx_power`), `rssi`, `espFreeHeap`, PSRAM totals,
  `fsUsedBytes`, `fsTotalBytes`, uptime.
- `GET /api/status` — the compact render / rotation snapshot.

---

## Bug-flagged settings at a glance

The following keys are accepted by PATCH and persisted to NVS but are
**not yet honored** by the v4 firmware as of the commit pinned at the
top of this file. Other agents are fixing these in parallel.

(empty as of 2026-04-26 — `poolLogosUrl` was wired up by the runtime
logo fetcher delivered in `f9048e9`, which closed out bd
`btclock_v4-5yi`. Every schema key now has a read site outside
`components/settings/`.)

bd `btclock_v4-7da` cleared the last seven honestly-dead keys on
2026-04-26: `wpTimeout`, `ledTestOnPower`, `inverseButtons`,
`enableDebugLog`, `minSecPriceUpd`, `suffixShareDot`, plus a `txPower`
boot re-read. The keys previously listed here as bug-flagged but
actually wired — `mdnsEnabled`, all `fl*`, `luxLightToggle`,
`suffixPrice`, `mowMode`, `refrScrnChange`, `fullRefreshMin`,
`stealFocus` — were removed at the same time after a triage pass
found their read sites.

"Not yet honored" here means `grep` across `components/` and `main/`
surfaces no read site outside `components/settings/` itself. The PATCH
round-trips cleanly; the effect is simply absent.

---

## Defaults audit (v3 parity)

Defaults below were sourced from
`btclock_v3_fci/src/lib/system/defaults.hpp` on 2026-04-24 and wired
through `FieldSpec::default_*` in `schema.hpp`. Each row shows the
v3 macro value, the v4 value *before* this sweep (mostly 0 / "" / false
because the schema carried no defaults), and the v4 value *after*. Rows
where v4 already matched v3 are omitted.

| Key | v3 | v4 before | v4 after |
|---|---|---|---|
| `bitaxeHostname` | `"bitaxe1"` | `""` | `"bitaxe1"` |
| `blockFeeDec` | `true` | `false` | `true` |
| `blockFlashColor` | `0xE04300` | `0` | `0xE04300` |
| `ceEndpoint` | `"ws-staging.btclock.dev"` | `""` | `"ws-staging.btclock.dev"` |
| `flAlwaysOn` | `true` | `false` | `true` |
| `flEffectDelay` | `15` | `0` | `15` |
| `flFlashOnUpd` | `true` | `false` | `true` |
| `flFlashOnZap` | `true` | `false` | `true` |
| `flMaxBrightness` | `2048` | `0` | `2048` |
| `flOffWhenDark` | `true` | `false` | `true` |
| `fontName` | `"antonio"` | `""` | `"antonio"` |
| `fullRefreshMin` | `60` | `0` | `60` |
| `gitReleaseUrl` | `"https://git.btclock.dev/api/v1/repos/btclock/btclock_v3/releases/latest"` | `""` | same as v3 |
| `hostnamePrefix` | `"btclock"` | `""` | `"btclock"` |
| `httpAuthUser` | `"btclock"` | `""` | `"btclock"` |
| `ledBrightness` | `128` | `0` | `128` |
| `ledFlashOnZap` | `true` | `false` | `true` |
| `ledTestOnPower` | `true` | `false` | `true` |
| `localPoolHost` | `"umbrel.local:2019"` | `""` | `"umbrel.local:2019"` |
| `luxLightToggle` | `128` | `0` | `128` |
| `mcapBigChar` | `true` | `false` | `true` |
| `mdnsEnabled` | `true` | `false` | `true` |
| `mempoolInstance` | `"mempool.space"` | `""` | `"mempool.space"` |
| `mempoolSecure` | `true` | `false` | `true` |
| `minSecPriceUpd` | `30` | `0` | `30` |
| `miningPoolName` | `"ocean"` | `""` | `"noderunners"` (v4 deliberately diverges from v3 — paired with `poolGlobalStats=true` for credentialless first-boot UX) |
| `poolGlobalStats` | `false` | `false` | `true` (v4 deliberately diverges from v3 — paired with `miningPoolName="noderunners"`) |
| `miningPoolUser` | `"38Qkkei3…TFy"` | `""` | `"38Qkkei3…TFy"` |
| `nostrPubKey` | `"6423171…6288"` | `""` | `"6423171…6288"` |
| `nostrRelay` | `"wss://relay.primal.net"` | `""` | `"wss://relay.primal.net"` |
| `nostrZapPubkey` | `"b5127a0…c422"` | `""` | `"b5127a0…c422"` |
| `otaEnabled` | `true` | `false` | `true` |
| `poolLogosUrl` | `"https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main"` | `""` | same as v3 |
| `scrnRestoreZap` | `true` | `false` | `true` |
| `tzString` | `"Europe/Amsterdam"` | `""` | `"Europe/Amsterdam"` |
| `useBlkCountdown` | `true` | `false` | `true` |
| `useMscwTime` | `true` | `false` | `true` |
| `verticalDesc` | `true` | `false` | `true` |
| `wpTimeout` | `15*60` (900 s) | `0` | `900` |

Drift found and corrected during this sweep:

- Every `bool` field that v3 defaulted to `true` was defaulting to `false`
  in v4 — the previous EmitField used a hardcoded zero-default regardless
  of the key.
- Same for every `uint` that v3 gave a non-zero initial value
  (`ledBrightness`, `blockFlashColor`, `fullRefreshMin`, `minSecPriceUpd`,
  `wpTimeout`, `flMaxBrightness`, `flEffectDelay`, `luxLightToggle`).
- Every string key that had a non-empty v3 default was reporting `""`.

Deliberate exclusions:

- `httpAuthPass` / `otaPass` keep an empty-string default in the GET
  surface because v3 also zeroed these in `settings.cpp` (the raw
  password must never ship to the client). The presence indicator is
  `httpAuthPassSet` / `otaPassSet`.
- `gmtOffset` removed entirely (bd `btclock_v4-9rx`) — see the Time
  zone section above.
- `wifiRebootMin` is v4-only (no v3 parallel); the default `10`
  carries over from the Arduino `checkWiFiConnection()` ten-minute
  fallback cadence.
