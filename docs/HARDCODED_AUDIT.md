# Hardcoded values audit

Snapshot date: 2026-04-26
Branch / commit: `95b3257`

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

### Boot splash text hardcoded — v3 `displayText` pref dropped (P2)

- **Status: open** (bd btclock_v4-592).
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

