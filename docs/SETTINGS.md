# BTClock settings reference

All settings live under the NVS namespace `settings` and are exposed via
`GET /api/settings` and `PATCH /api/settings`. Changes via PATCH persist
to NVS and take effect without reboot unless the "Notes" column says
otherwise. A PATCH that writes a reboot-only field still persists the
value immediately; the response is `{"rebootRequired": true}` so the
WebUI can prompt the user.

Source of truth for the list is
`components/settings/include/settings/schema.hpp` (`kFields` — 85 scalar rows),
plus a handful of special-case keys (`dnd`, `screens`, `actCurrencies`,
`timePerScreen`, `invertedColor`, `txPower`) handled directly in
`components/settings/settings_api.cpp::ApplyPatch()`.

The WebUI mirrors `kFields` into a generated TypeScript module via
`scripts/generate-settings-meta.py` (in the WebUI repo). Whenever a
field is added, removed, or its `boot_only` flag flips here, run
`pnpm generate:settings-meta` in the WebUI checkout to regenerate
`src/lib/types/settings.generated.ts`. A unit test in the WebUI
(`settings.generated.spec.ts`) cross-checks "(restart required)"
labels against this metadata so a stale label fails CI before it
reaches a device.

### WebUI `minFirmware`

The WebUI bundle declares a semver **`minFirmware`** in
[`data/src/lib/manifest.json`](../data/src/lib/manifest.json).
Raise it when this checkout's settings UI or cold-start parsers **require**
fields or JSON shapes that older firmware cannot emit (see also the comment
block in `data/gzip_build.py`). The value is copied into `build_gz/www/manifest.json`
by `pnpm build:gz` and served from the device as `/manifest.json`; System Info
compares the running firmware `gitRev` against it.

Defaults in the table below come from `components/settings/include/settings/schema.hpp`'s
`FieldSpec::default_*` fields, ported from the old firmware's
`src/lib/system/defaults.hpp`. A fresh install (NVS wiped, or a key
never PATCHed) surfaces these values.

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
the old firmware). Validation failures return `400 Bad Request` with
a structured `{"error": "<token>"}` body. The WebUI's
`parseSettingsError` helper splits the token on `:` and uses the left
half to identify the offending field (so it can pop the relevant
`CollapseCard` open and scroll the input into view) and the right half
to localise the reason. The known vocabulary today:

| `error` token | Meaning | Where it fires |
|---|---|---|
| `json` | Body is not valid JSON. | Top-level parser. |
| `not_object` | Body parsed but the root is not a JSON object. | Top-level parser. |
| `bad body` | `Content-Length: 0` or > 16 KiB. | HTTP wrapper before parse. |
| `<key>:bad_type` | Known key with the wrong JSON type (e.g. `"true"` for a `bool` field, `"30"` for a `uint`). | Generic `kFields` walk. |
| `<key>:bad_length` | String exceeds NVS / hardware bound (e.g. miningPoolUser too long). | Per-key validators. |
| `<key>:bad_hex` | String fails per-key hex validation (e.g. `nostrZapPubkey` not 64 hex chars). | Per-key validators. |
| `<key>:bad_scheme` | URL string with a forbidden scheme (e.g. `nostrRelay` not `wss://` / `ws://`). | Per-key validators. |
| `<key>:unknown` | Enum-typed string with an unknown value (`fontName`, `miningPoolName`). | Per-key validators. |
| `range:<key>` | Numeric or numeric-bounded string out of `[min,max]`. **Note the inverted shape** — the field name is on the right of the colon, not the left. The WebUI's `parseSettingsError` handles both forms. | Generic `kFields` walk for `kUint` / `kUChar`. |
| `screens:partial_order` | Array contains some entries with `order` and some without. PATCH must be all-or-nothing. | `screens[]` validator. |
| `screens:bad_entry` | Entry is not a JSON object, or is missing `id`. | `screens[]` validator. |
| `screens:unknown_id` | `screens[i].id` does not match a defined screen. | `screens[]` validator. |
| `screens:dup_id` / `screens:dup_order` | Two entries share an `id` or `order` value. | `screens[]` validator. |
| `screens:order_range` | `order` outside `[0, screens.length)`. | `screens[]` validator. |
| `screens:incomplete` | Reorder PATCH did not list every defined screen. | `screens[]` validator. |
| `currency:not_string` | `actCurrencies[]` entry is not a string. | `actCurrencies[]` validator. |
| `dnd:range` | `startHour`/`startMinute`/`endHour`/`endMinute` outside `[0,23]`/`[0,59]`. | DND nested-object validator. |

Every `kString` field is additionally subject to two generic checks
applied before the per-key validators above (commit `0ab174e`):

- **Length cap** — values longer than 256 bytes are rejected. Per-field
  tighter caps can be set via `FieldSpec.max_value`; the 256-byte
  default kicks in when none is specified. Longer-than-cap PATCHes
  surface as `<key>:bad_type`.
- **Control-byte rejection** — bytes < `0x20` or `== 0x7F` are
  refused. UTF-8 high-bit sequences (e.g. `é`) pass through. Also
  surfaces as `<key>:bad_type`.

The PATCH `200` body is empty when no `boot_only` field was touched,
or `{"rebootRequired": true}` when at least one was. The WebUI uses
this to surface a "restart required" banner without firing a reboot
itself.

## Grouping rationale

Settings are grouped by the subsystem that owns them rather than by
schema type, so the reader can find "where does the Bitaxe host go?"
without scanning all booleans. Each group ends with any special-case
derived keys the WebUI uses to render that section.

---

## Display

| Key | Type | Default | Description | Notes |
|---|---|---|---|---|
| `fontName` | string (enum) | `"antonio"` | Font family used by label / digit / small-chars / unit screen roles. | Values come from `availableFonts` in GET (`antonio`, `oswald`, `inter`, `sourceSerif`, `merriweather`, `bitter`, `atkinson`, `antonioSemiBold`, `antonioBold`, `oswaldBold`, `interBold`, `sourceSerifBold`, `merriweatherBold`, `bitterBold`, `atkinsonBold`, `openRunde`, `roboto`, `robotoBold`, `notoSans`, `notoSansBold`, `ubuntu`, `ubuntuBold`, `azeret`). The three Antonio cuts share letterforms but step up in stroke weight (wght=400 / 600 / 700); the non-Antonio `*Bold` ids select the already-embedded bold cut directly (no extra font asset). `openRunde`, `roboto`, `notoSans`, and `ubuntu` ship as dedicated base+bold pairs (Ubuntu's base cut is the upstream Medium face, used as the Semibold-equivalent body cut). Azeret Mono is the catalogue's only monospaced face and ships Regular only (`azeret`); the SemiBold cut (`azeretSemiBold`, id 23) was retired to recover Rev A flash headroom — its bold slot doubles the Regular like base Antonio. The Regular subset includes U+20BF (`₿`). Rebound live via the `on_font_changed` hook, which marks the screen dirty for a repaint. PATCH validates against the catalogue. Devices upgrading from a build that stored the retired `"dejavu"` or `"azeretSemiBold"` value fall back to `"antonio"` at boot via the validation walk. |
| `digitFontPx` | uint (80..220) | `180` | Pixel height for the big digit glyphs on data screens — block height, halving countdown, BTC ticker, time, etc. | Bounds leave horizontal headroom for Antonio's widest digit ink within the 122 px short axis (≈0.55 × px ink-to-height, 220 → ~121 px). `ScreenManager::Render` reads this each frame and pushes it through `SetGlobalDigitPx`; `on_settings_patched` marks the screen dirty so the next paint repaints with the new size. |
| `labelFitPct` | uint (25..100) | `100` | Split-label fit target width as a percentage of available panel width. `100` preserves historical full-width auto-fit; lower values reserve extra side whitespace (`75`, `50`, etc.) while still running the same auto-fit logic. | Read live by `ScreenManager::Render` and applied via `SetGlobalLabelFitPercent` before panel paint. Values are clamped defensively to 1..100 in the renderer; schema enforces 25..100 on PATCH. |
| `invertedColor` | bool | `false` | White-on-black when `true`, black-on-white when `false` (default — matches the natural EPD polarity, where un-driven pixels are white). | PATCH side-writes `fgColor` / `bgColor` to keep EPD polarity consistent. Applied live: `EpdSetGlobalInverted` + `ScreenManager::MarkDirty`. |
| `fgColor` | uint (hex RGB) | `0xFFFF` | EPD foreground colour. | Auto-updated when `invertedColor` is PATCHed; schema does not expose it as a PATCH key, but GET emits it. |
| `bgColor` | uint (hex RGB) | `0x0000` | EPD background colour. | Same auto-update rule as `fgColor`. |
| `fullRefreshMin` | uint | `60` | Minutes between mandatory full-refresh cycles on the EPDs. | Range 0..1440. Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `refrScrnChange` | bool | `false` | Force a full EPD refresh on every screen change. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `verticalDesc` | bool | `true` | Rotate label panels 90° CCW so text reads along the panel's long axis. | Honored by `PaintSlotIntoFb` for `kLabel` / `kLabelSplit` slots (ports v3 `splitText`'s verticalDesc branch). Applied live via `ScreenManager::LoadScreenRenderPrefs`. |
| `mcapBigChar` | bool | `true` | Use big-character layout for market-cap and supply screens. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `supplyPercent` | bool | `false` | Render bitcoin supply as a percentage of 21M rather than absolute BTC. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `useBlkCountdown` | bool | `true` | Halving screen: count in blocks remaining, not days. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `useMscwTime` | bool | `true` | Show the sats-per-dollar ("Moscow Time") flip layout. | Honored by `ScreenManager::LoadScreenRenderPrefs`. |
| `priceSymMode` | uint (0..2) | `0` | BTC price-row marker: **0** off, **1** Satoshi Symbol glyph (PUA font), **2** ₿ via digit font. NVS key `priceSymMode` (≤15 chars). Replaces legacy mutually-exclusive `useSatsSymbol` / `useBtcSymbol`; migrated once at boot from those keys when `priceSymMode` was never written. | Honored by `ScreenManager::ReadRenderPrefs` (`RenderPrefs::use_sats_symbol` / `use_btc_symbol`). Out-of-range stored values clamp to 0. |
| `satsVariant` | uint (0..15) | `7` | Index into the 16 sats-symbol glyphs at U+E000..U+E00F of the SatoshiSymbol font. Visible on the moscow-time and nostr-zap screens when `priceSymMode === 1`. The WebUI Settings page renders a visual picker with all 16 glyphs inline; pick by clicking, or PATCH the integer index live. See [the contact sheet](img/fonts/sats_variants.png) for the full set. | Range-checked by the schema (kUint min=0 max=15). The `on_sats_variant_changed` hook in main calls `ScreenManager::SetSatsVariant` + `MarkDirty()` so the new glyph paints on the next render without a reboot. Stored in the `settings` namespace; legacy NVS values under `ui/sats_variant` are picked up at boot for one-time migration. |
| `blockFeeDec` | bool | `true` | Show decimal sats/vB on the fee-rate screen when the data source reports a precise value. | Honored in `ScreenManager` rendering path. |
| `suffixPrice` | bool | `false` | Use k/M suffixes on the price screen. | Honored by `ScreenManager::ReadRenderPrefs` and threaded through `RenderBtcPriceScreen` / `BuildBtcPrice`. |
| `decimalShareDot` | bool | `false` | When `true`, the decimal-point dot shares the digit panel before it instead of taking its own panel — frees one panel for an extra digit. Applies anywhere the layout includes a `.`: the BTC price K/M-suffix path, market-cap big-chars, and the sub-1 sat-per-currency `0.dddd` path on `SATS/<CCY>`. When `false` (default), the dot occupies its own panel. **Renamed** 2026-05-05 from `suffixShareDot` (the old name implied K/M suffix only). Legacy NVS values are auto-migrated at boot — see `init_screen_manager.cpp`. | Honored by `LayoutBtcPriceSuffixStrings`, market-cap, and `ComputeSatsPerCurrencyLayout` via `ScreenManager::ReadRenderPrefs`. |
| `mowMode` | bool | `false` | "Mow mode" price formatting — renders the BTC/fiat price in **millions of fiat per BTC** (`$X.XM`) instead of the raw integer. Named after Samson Mow, who popularised quoting Bitcoin in millions. | Honored by `ScreenManager::ReadRenderPrefs` and `LayoutBtcPriceSuffixStrings`. |
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
| `hostnamePrefix` | string | `"btclock"` | Hostname prefix used for mDNS + DHCP. The MAC suffix is appended. | Live for mDNS via `on_mdns_changed` (calls `ReinitMdns` to free + re-publish under the new name). DHCP keeps the previous lease until the next renewal. |
| `mdnsEnabled` | bool | `true` | Advertise `_http._tcp` and `_btclock._tcp` over mDNS. | Live via `on_mdns_changed` — toggling false frees the responder, true re-publishes. Honored in `main/app/boot/init_mdns.cpp`. |
| `wifiRebootMin` | uint | `10` | Minutes of continuous STA disconnect before a soft reboot. `0` disables. | Range 0..120. Live — the wifi_guard tick re-reads on every fire. |
| `wpTimeout` | uint | `900` (15 min) | WiFiManager captive-portal timeout (seconds). | Range 0..3600. Reboot required. After this many seconds in AP-provisioning mode the device reboots so the next boot retries STA — gated on `wifiConfigured` being true (set once the user submits creds), so a never-provisioned device sits in the portal indefinitely. |
| `txPower` | int | device default | WiFi TX power in quarter-dBm. `-1..78` valid; `80` is a sentinel that clears the override. | Live. Applied via `esp_wifi_set_max_tx_power`. Re-applied at boot in `init_network.cpp` between `esp_wifi_start` and `Connect`, mirroring the same `IsValidWifiTxPower` range gate. Queried back on `/api/status` via `esp_wifi_get_max_tx_power`. |
| `wifiConfigured` | bool | `false` | Set by the provisioning flow once STA credentials are captured. | Not PATCH-settable in practice — internal flag. |
| `proxyEnabled` | bool | `false` | Master toggle for the outbound HTTP CONNECT / SOCKS proxy. | All proxy fields are live — a PATCH applies to the next outbound connection and existing WS sessions reconnect automatically. See [Proxy](PROXY.md) for the full reference. |
| `proxyType` | uchar | `0` | `0`=none, `1`=HTTP CONNECT, `2`=SOCKS4, `3`=SOCKS4a, `4`=SOCKS5. | Bounded 0..4; the fixed-bound enum mapping is documented in [Proxy](PROXY.md). |
| `proxyHost` | string | `""` | Proxy hostname or IP. | Required when `proxyEnabled` is true. |
| `proxyPort` | uint | `1080` | Proxy port, 1..65535. | Default `1080` matches the SOCKS convention. |
| `proxyUser` | string | `""` | Username for SOCKS5 / HTTP CONNECT auth. | SOCKS4/4a have no auth frame — ignored for those types. |
| `proxyPass` | string | `""` | Password for SOCKS5 / HTTP CONNECT auth. | Suppressed in GET (mirrors `httpAuthPass`); `proxyPassSet: bool` indicates whether one is stored. |
| `proxyBypass` | string | `"*.local,192.168.*,10.*,127.0.0.1"` | Comma-separated host globs that skip the proxy and connect directly. | Glob syntax: exact, `*.suffix`, or `prefix.*`. Case-insensitive. |

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
| `flOffOnDnd` | bool | `true` | Suppress the frontlight while DND is active. | Rev B only. Default-on preserves the v3 hardcoded behaviour. Edge-detected on the main loop's 1 Hz tick so manual + time-based DND transitions fade the panel without other events flowing. Set false to keep the LED ring muted by DND while the panel stays under normal user/ambient control. Read at boot in `init_hardware.cpp`; live PATCH applied via `on_frontlight_changed`. |
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

Both `gmtOffset` and the `tzOffset` (minutes) PATCH alias were removed in v4. The firmware drives the clock from POSIX TZ strings via `setenv("TZ", ...) + tzset()`, and the old offset pref was never read back. Legacy clients that still send `gmtOffset` / `tzOffset` get a silent no-op (the rest of the PATCH body still applies).

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

## Read-only fields (GET only — not PATCH-settable)

These appear in the `GET /api/settings` response but are not writable
via PATCH. They're derived from the running firmware and the device
context.

| Key | Type | Source | Description |
|---|---|---|---|
| `hostname` | string | Wifi STA | Resolved `<hostnamePrefix>-<mac>` hostname. |
| `ip` | string | Wifi STA | Current STA IP, empty in AP mode. |
| `hwRev` | string | compile-time | Human-readable hardware revision (e.g. `"Rev B"`). |
| `fsRev` | string | compile-time | LittleFS bundle version string. Stamped from the firmware's `PROJECT_VER` so a build that produces firmware + WebUI from one tree reports `fsRev == gitRev`. |
| `gitRev` | string | compile-time | Git SHA of the firmware. Omitted when unknown. |
| `gitTag` | string | compile-time | Git tag, when the build was tagged. Omitted when empty. |
| `lastBuildTime` | int | compile-time | Firmware build time as Unix seconds (UTC). Omitted when the compiler emitted an unparseable `__DATE__`. |
| `numScreens` | int | board config | Number of EPD panels on this board (3 on Rev A / Rev B, 7 on V8). Emitted identically by `GET /api/status` so the WebUI's custom-text `maxlength` agrees with both endpoints. Not related to the screen-rotation slot count. |
| `screens[]` | array of objects | `kScreenKinds` + NVS | Per-screen rotation catalogue, see the Display section. |
| `availableFonts[]` | array of `{ id, hasBtcSymbol }` | `catalog_available_font_catalog.gen.hpp` | Bundled digit fonts the WebUI picker lists. `hasBtcSymbol` is baked when fonts are regenerated (per-TTF cmap), so the ₿ marker toggle can track `fontName` immediately without a settings refetch. |
| `availablePools[]` | string array | `AvailablePoolNames()` | Mining-pool ids the WebUI picker shows. |
| `availableCurrencies[]` | string array | `kAvailableCurrencies` | ISO codes the price feed can serve. |
| `hasFrontlight` | bool | board capability | Whether this board has a PCA9685 frontlight (Rev B only today). |
| `hasLightLevel` | bool | board capability | Whether a BH1750 ambient sensor is populated (Rev B only today). |
| `lightLevel` | number (lux) | BH1750 | Current lux reading. Only emitted when `hasLightLevel` is true. |
| `httpAuthPassSet` | bool | NVS presence | `true` iff `httpAuthPass` is non-empty. |
| `otaPassSet` | bool | NVS presence | `true` iff `otaPass` is non-empty. |
| `miningPoolUserSet` | bool | NVS presence | Emitted **only** when the active `miningPoolName` is a secret-key pool (`viabtc`, `foundry_usa`); `true` iff `miningPoolUser` is non-empty. For public-info pools the raw `miningPoolUser` rides plaintext and this companion is omitted. |
| `timerRunning` | bool | runtime | Rotation-timer live state. |
| `invertedColor` | bool | NVS | Default `false` (black-on-white) when the key is absent. |

Two additional GET-only bodies are surfaced by sibling endpoints that
the WebUI calls alongside `/api/settings`:

- `GET /api/system/status` — returns `txPower` (queried live via
  `esp_wifi_get_max_tx_power`), `rssi`, `espFreeHeap`, PSRAM totals,
  `fsUsedBytes`, `fsTotalBytes`, uptime.
- `GET /api/status` — the compact render / rotation snapshot.

---

## Honoured-status guarantee

Every schema key listed above has a read site outside
`components/settings/` — i.e. PATCHing it actually changes device
behaviour, not just NVS contents. The "Notes" column on each row
points to the consumer (`ScreenManager::ReadRenderPrefs`,
`on_frontlight_changed`, `init_hardware.cpp`, etc.) so the wiring is
auditable from this page.

Two exceptions, intentional and documented in their rows:

- `httpAuthPass` / `otaPass` — secrets are never echoed; presence is
  surfaced via the `…Set` companion booleans.
- `wifiConfigured` — internal provisioning-flow flag, set by the AP
  flow rather than by user PATCH.
