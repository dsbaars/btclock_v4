# Hardcoded values audit

Snapshot date: 2026-04-25
Branch / commit: `d723ae3`

## Methodology

A finding is in scope when **(a)** the value is a literal in v4 source, AND
**(b)** v3 (`btclock_v3_fci/`) exposed it as a settings field or the v4
settings schema (`components/settings/include/settings/schema.hpp`)
already declares a pref key for it that no production code path
consumes. Out of scope: panel byte layouts, GPIO assignments, font
glyph metrics, partition offsets, internal protocol bytes, and
reasonable timeout defaults that have no v3 settings analogue or
operator-facing UX justification.

`docs/SETTINGS.md` already enumerates many "declared but not honored"
keys; this audit narrows that list to the subset where the consumer is
either a hardcoded literal in source or an unrelated NVS namespace,
and adds the small set of fully-hardcoded values that have no schema
slot at all.

## Findings

### 1. Nostr data-source pubkey + relay read from the wrong NVS namespace (P2)

- **Location**: `main/sources/sources.cpp:39-42`
  ```
  Prefs nostr_prefs("nostr");
  const bool enable = nostr_prefs.GetBool("enable", false);
  const std::string relay = nostr_prefs.GetString("relay", "");
  const std::string pub = nostr_prefs.GetString("pub", "");
  ```
- **Current value**: namespace `"nostr"`, keys `"enable"`, `"relay"`,
  `"pub"`.
- **Why configurable**: `components/settings/include/settings/schema.hpp:186-188`
  declares `kNostrPubKey` / `kNostrRelay` in the `"settings"` namespace.
  The WebUI PATCHes those keys; this reader looks at a separate
  namespace, so user changes never reach the data-source builder.
  Schema marks the keys boot-only, which fits — only the namespace +
  key strings need to align.
- **Suggested setting**: re-use `prefs::kNostrRelay` /
  `prefs::kNostrPubKey` from the `prefs::kSettingsNs` namespace; gate
  enable on `prefs::kDataSource == 2` (Nostr data-source enum value)
  per `defaults.hpp::DataSourceType`.
- **bd issue**: btclock_v4-aw5

### 2. Zap-listener relay + pubkey read from the wrong NVS namespace (P2)

- **Location**: `main/app/boot/init_zap_listener.cpp:31-37`
  ```
  Prefs zap_prefs("nostr");
  const bool zap_enable = zap_prefs.GetBool("zapEnable", true);
  const std::string zap_relay_url =
      zap_prefs.GetString("zapRelay", "wss://relay.primal.net");
  const std::string zap_pub = zap_prefs.GetString("zapPubkey", ...);
  ```
- **Current value**: namespace `"nostr"`, keys `"zapEnable"`,
  `"zapRelay"`, `"zapPubkey"`.
- **Why configurable**: schema (`schema.hpp:188-191`) declares
  `kNostrRelay` and `kNostrZapPubkey` in `"settings"` and marks
  `nostrRelay` boot-only. `kNostrZapNotify` is correctly consumed
  from `"settings"` on line 54 of the same file, so the zap-screen
  toggle works but a relay or pubkey PATCH is silently ignored
  until reboot AND would still hit the wrong namespace post-reboot.
- **Suggested setting**: same as #1 — read `kNostrRelay` and
  `kNostrZapPubkey` from `kSettingsNs`. v4-only `flashOnZap` /
  `flFlashOnZap` keys here also drift from the schema's
  `kFlFlashOnZap` / `kLedFlashOnZap`; reconcile in the same patch.
- **bd issue**: btclock_v4-q1l

### 3. Boot splash text hardcoded — v3 `displayText` pref dropped (P2)

- **Location**: `main/boot_ui.cpp:8`
  ```
  constexpr const char* kSplashLetters = "BTCLOCK!";
  ```
- **Current value**: literal `"BTCLOCK!"` (last char only used on
  8-panel V8).
- **Why configurable**: v3 `epd.cpp:151` reads
  `preferences.getString("displayText", DEFAULT_BOOT_TEXT)`; the WebUI
  could change it via `/api/show/text` and v3 used the same NVS slot
  as both the splash default and the user-show buffer
  (`pref_keys.hpp::DisplayText`, `defaults.hpp::DEFAULT_BOOT_TEXT`).
  No `displayText` key exists in v4 at all (no schema slot, no NVS
  reader).
- **Suggested setting**: add `kDisplayText` (string, default
  `"BTCLOCK"`, padded with spaces to NUM_SCREENS) to `pref_keys.hpp`
  + `schema.hpp` (boot-only — splash only paints once).
- **bd issue**: btclock_v4-592

### 4. LED brightness / disable / flash-on-update use a separate "led" namespace; schema keys are dead (P2)

- **Location**: `main/io/led_controller.cpp:31-35` declares its own
  `"led"` namespace with keys `brightness`, `disable`, `flashUpdate`,
  `blockFlashCol`. `LoadPrefs()` reads only those.
- **Current value**: WebUI PATCHes `kLedBrightness="ledBrightness"`,
  `kDisableLeds="disableLeds"`, `kLedFlashOnUpd="ledFlashOnUpd"`
  into `kSettingsNs="settings"`; the LED task never reads them.
- **Why configurable**: v3
  (`btclock_v3_fci/src/lib/system/defaults.hpp:8-11`) wired all four as
  user-visible settings with non-zero defaults
  (`DEFAULT_LED_BRIGHTNESS=128` etc.). v4 declares them in the schema
  with the same defaults but never bridges to the LED controller
  (only `kBlockFlashColor` is mirrored at boot, in
  `init_boot_leds.cpp:46`).
- **Suggested setting**: at boot, mirror `settings/ledBrightness` →
  `led/brightness`, `settings/disableLeds` → `led/disable`,
  `settings/ledFlashOnUpd` → `led/flashUpdate` (same pattern as the
  existing `blockFlashColor` mirror), and add live-update hooks.
- **bd issue**: btclock_v4-xfm

### 5. Frontlight runtime settings persist but PATCH does not apply live (P2)

- **Location**: `main/app/boot/init_hardware.cpp:97-133` reads
  `kLuxLightToggle`, `kFlOffWhenDark`, `kFlMaxBrightness`,
  `kFlEffectDelay` once at boot. `init_control_api.cpp:174` only
  marks the screen dirty on PATCH; no frontlight forwarder.
- **Current value**: schema flags these as `boot_only=false`
  (`schema.hpp:114-122`), so the WebUI doesn't prompt for reboot —
  but a PATCH has no visible effect until reboot.
- **Why configurable**: v3 read these on every paint of the ambient
  loop (`btclock_v3_fci/src/main.cpp:31-47`). v4 removed the per-tick
  read for performance but the PATCH-time hook to push new values
  into `FrontlightController` was never added.
- **Suggested setting**: add
  `on_frontlight_settings_changed(lux_threshold, off_when_dark,
  max_brightness, effect_delay)` to `ControlServer::Config`, fired
  from `ApplyPatch()` whenever any of those four keys is touched.
  Forward to `FrontlightController::Set{LuxThreshold,OffWhenDark,
  ConfiguredBrightness,EffectDelay}` (already exposed).
- **bd issue**: btclock_v4-7xv

### 6. Frontlight enable / disable / flash-on-update are fully dead (P2)

- **Location**: schema declares `kFlAlwaysOn` (`schema.hpp:114`),
  `kFlDisable` (`:115`), `kFlFlashOnUpd` (`:117`); no read site
  exists in `main/` or `components/`.
- **Current value**: `false`/false/`true` defaults; PATCH persists
  but the controller behaviour is hardcoded.
  `frontlight_controller.cpp` keeps the light always-on by default
  and there is no flash-on-update path at all.
- **Why configurable**: v3 had `flAlwaysOn`, `flDisable`,
  `flFlashOnUpd` as first-class settings each driving different
  behaviour
  (`btclock_v3_fci/src/lib/system/defaults.hpp:21,52`). v4's
  frontlight code only wires the four "ambient" prefs above.
- **Suggested setting**: wire all three through the same forwarder
  added in finding #5.
- **bd issue**: btclock_v4-63p

### 7. wpTimeout, ledTestOnPower, suffixShareDot, mowMode, suffixPrice, ... unread schema keys (P2)

- **Location**: schema declares each in `schema.hpp`; grep across
  `main/` + `components/` (excluding `settings/` itself) shows zero
  read sites.
  - `kWpTimeout` (`:227`) — v3 captive-portal timeout, v4 portal
    runs without timeout.
  - `kLedTestOnPower` (`:162`) — v3 default true; v4 always runs
    boot rainbow.
  - `kInverseButtons` (`:149`) — v3 swapped prev/next at runtime.
  - `kSuffixShareDot` (`:209`), `kSuffixPrice` (`:208`),
    `kMowMode` (`:183`) — already noted in `docs/SETTINGS.md` "bug
    flagged" list as renderer logic v4 hasn't ported.
  - `kEnableDebugLog` (`:109`), `kTxPower` (`:227` style; in
    `pref_keys.hpp:106`) — v4 reads them only via `/api/status`
    (live), never from NVS at boot, so a power cycle drops them.
  - `kPoolLogosUrl` (`:199`) — only the WebUI consumes it; no
    firmware impact, fine to leave but consider hiding from GET.
  - `kMinSecPriceUpd` (`:174`) — v3 throttled the price-update
    pipeline; v4's WSS push doesn't need throttling but the WebUI
    still surfaces a number with no effect.
- **Why configurable**: v3 honored each of these. The WebUI today
  reports the saved value back, so users assume it works.
- **Suggested setting**: per-key resolution — wire most up; remove
  the schema slots that v4 architecturally cannot honor (e.g.
  `minSecPriceUpd` if the WSS feed is the only data source).
  Group similar fixes per area to keep PRs reviewable.
- **bd issue**: btclock_v4-7da (umbrella; sub-fixes can be filed as
  it lands)

### 8. mempoolInstance / mempoolSecure declared but unused — `dataSource=1` not implemented (P3)

- **Location**: schema declares `kMempoolInstance` /
  `kMempoolSecure` (`schema.hpp:172-173`); no read site outside
  `settings_api.cpp`. `sources/sources_uri.cpp:26` falls back to the
  public BTClock WSS for any `dataSource != 2`.
- **Current value**: defaults `"mempool.space"` / `true`.
- **Why configurable**: v3 used these for the THIRD_PARTY_SOURCE
  data-source variant (mempool.space WS + Kraken price feed);
  `block_notify.cpp:122` reads them every poll. v4 logs
  `"dataSource=%d not implemented"` when the user picks 1.
- **Suggested setting**: not a separate fix — already tracked by
  `btclock_v4-1xc` (mempool+kraken data source). Mention in the
  same epic; do not double-file.
- **bd issue**: existing `btclock_v4-1xc` (related; not duplicated)

### 9. Mining-pool poll cadence is hardcoded (P3)

- **Location**: `components/mining_pool_common/include/mining_pool_common/pool_base.hpp:73`
  ```
  virtual uint32_t poll_interval_ms() const { return 60 * 1000; }
  ```
- **Current value**: 60 s for every pool. No override in any
  `mining_pool_*` subclass (`grep poll_interval_ms` returns only
  the base).
- **Why configurable**: v3 had no per-user setting either, but ran
  on a 1-minute timer (`mining_pool_stats_fetch.cpp` triggered by
  the stats timer). Nothing in v3 lets the user shorten this. UX
  motivation is thin — surfacing it as a setting would make the
  rate-limit policy visible without committing to the v3 default.
- **Suggested setting**: optional — add `kPoolPollSec` (uint, range
  10..3600, default 60) to schema. Skip if maintainer prefers the
  60 s default to remain a constant.
- **bd issue**: btclock_v4-gku

### 10. Bitaxe poll interval is hardcoded (P3)

- **Location**: `components/bitaxe/include/bitaxe/bitaxe_source.hpp:35`
  ```
  uint32_t poll_interval_ms = 10 * 1000
  ```
  default in `BitaxeSource(...)` ctor; `MakeBitaxeSource()`
  (`bitaxe_source.cpp:185`) uses the default.
- **Current value**: 10 s.
- **Why configurable**: a Bitaxe on a tight LAN can be polled
  faster; on a flaky 2.4 GHz it should be slower. v3 had no
  setting either, so this is borderline — file a P3 to expose it
  per `kBitaxePollSec` (uint, range 5..300, default 10).
- **bd issue**: btclock_v4-6hq

## Out of scope (intentionally hardcoded)

- Mining-pool API base URLs (`pool.braiins.com`,
  `api.ocean.xyz`, `solo.ckpool.org`, `pool.satoshiradio.nl`,
  `pool.noderunners.network`, `public-pool.io`,
  `pool.gobrrr.me`). Each pool's URL is a contract with that
  pool's API; the **selector** (`miningPoolName`) is already
  configurable. v3 hardcoded the URLs the same way.
- Bitaxe URL (`http://<host>/api/system/info`) — `<host>` is
  `kBitaxeHostname`-driven; the path is the AxeOS API contract.
- WS reconnect cadence (5 s linear) and ping-interval (20-30 s)
  in `btclock_data.cpp:29` and `nostr/relay_client.cpp:24-26`.
  Fine defaults; v3 didn't expose them; no UX motivation.
- Provisioning portal URL `/setup`, AP-mode IP, `kBootPalette`
  (LED rainbow palette), boot-text per-panel layout — visual
  design / hardware contracts.
- Splash glyph dimensions (`220.0f, 100.0f, lfb.native_width - 12`
  in `boot_ui.cpp:35-37`) — pure layout, not config.
- Initial cached block height / price (v3 had
  `INITIAL_BLOCK_HEIGHT=925000` / `INITIAL_LAST_PRICE=99000`); v4
  blocks until the first WS push and degrades to "wait spinner"
  if it doesn't arrive — strictly better than a stale literal.

## Related, in flight

- `dataSource` + `ceEndpoint` / `ceDisableSSL` was wired up in
  commit `d723ae3` (the immediate parent of this audit). That work
  is complete — no follow-up needed for the WSS endpoint case.
- `screenOrder`, `actCurrencies`, `timerSeconds`,
  `invertedColor` polarity, font name, `hideLeadZero`,
  `suffixPrice`, `mowMode` (display/clock prefs) all landed in
  recent commits per `git log`. They're either honored already or
  flagged in finding #7 above for completeness.
