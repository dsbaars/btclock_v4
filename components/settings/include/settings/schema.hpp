// Schema metadata for every PATCH-accepted field in /api/settings.
// The WebUI POSTs a subset of these fields in any given PATCH; the
// handler loops the table and dispatches by kind. Kept pure-logic so
// host tests can exercise the validator without linking IDF.
//
// Mirrors the three arrays in the old firmware's
// src/lib/net/webserver/settings.cpp (strSettings / uintSettings /
// boolSettings), promoted to a typed table so we can attach extra
// flags (boot_only, range bounds) without another parallel list.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "settings/pref_keys.hpp"

namespace btclock {
namespace settings {

enum class FieldKind : uint8_t {
  kString,
  kUint,  // stored as NVS u32
  kInt,   // stored as NVS i32 — signed (tx power, gmt offset)
  kBool,
  kUChar,  // stored as NVS u8 (dataSource)
};

// Per-field defaults travel in the schema so GET /api/settings returns
// the same values the old firmware's src/lib/system/defaults.hpp exposed
// when a key has never been written. Without this the WebUI saw 0 / ""
// / false for anything the user hadn't PATCHed yet and would overwrite
// the NVS slot with the wrong thing on the first save. Three parallel
// defaults keep the struct POD-trivial; only the one matching FieldKind
// is consulted by EmitField.
struct FieldSpec {
  std::string_view key;  // NVS key + JSON field name (identical)
  FieldKind kind;
  // True when a change cannot take effect until reboot. The PATCH
  // handler returns {"rebootRequired": true} alongside 200 OK when
  // any boot_only field is touched.
  bool boot_only;
  // Optional bounds for uint/uchar fields. min==max==0 disables.
  uint32_t min_value;
  uint32_t max_value;
  // Defaults. Exactly one is read per kind:
  //   kBool       -> default_bool
  //   kUint/kInt  -> default_int (sign carried by kind)
  //   kUChar      -> default_int (clamped 0..255 by field range)
  //   kString     -> default_str
  // Unset values (0 / false / "") are legitimate defaults and match
  // v3's behaviour for keys where the old firmware never set one.
  bool default_bool = false;
  int32_t default_int = 0;
  std::string_view default_str = {};
};

// Field catalogue. Kept alphabetised for easy review.
//
// boot_only classification matches old-firmware reality as of the port
// date:
//   - WiFi/network identity fields (httpAuthEnabled, httpAuthUser,
//     httpAuthPass, otaPass, otaEnabled) require reboot — they drive
//     services started exactly once in setupWifi() / setupWebserver()
//     / ArduinoOTA.begin(). hostnamePrefix and mdnsEnabled used to be
//     in this set but bd btclock_v4-9ut wired on_mdns_changed so the
//     advertisement re-publishes live.
//   - Data-source fields (dataSource, mempoolInstance, mempoolSecure,
//     ceEndpoint, ceDisableSSL, localPoolHost) reboot because the
//     old firmware tears down the WS client and setupDataSource()
//     only runs at boot (it's legal to call again, but it leaves
//     dangling tasks in practice).
//   - invertedColor used to be in this set (EPD driver init) but
//     bd btclock_v4-5wj wired epd::SetGlobalInverted so it applies live.
//     fontName likewise: bd btclock_v4-5az dispatches kSetFont through
//     the ControlCommand queue (see main/app/event_loop.cpp), so the
//     four-pointer AppFonts swap happens on the main task without a
//     reboot. Keep the schema entry runtime so the PATCH response no
//     longer claims rebootRequired.
//   - Everything else takes effect live: LED brightness, DND, screen
//     visibility, countdown flags, etc.
//
// Range bounds only where the old firmware enforced them or where a
// bad value crashes hardware (brightness > 8-bit, hour > 23, etc.).
//
// Defaults track the v3 firmware's src/lib/system/defaults.hpp so a fresh
// install returns the same values the Arduino firmware did. Where v3
// had no default at all (plain preferences.getXxx(key) with the C++
// zero-init fallback), the spec entry leaves the default_* fields at
// their struct-init zero values. See docs/SETTINGS.md for the audit
// table.
inline constexpr std::array<FieldSpec, 81> kFields = {{
    // actCurrencies: comma-joined active ISO codes for the rotation.
    // Stored as a CSV string in NVS; GET emits it as an array (handled
    // out-of-band in BuildGetResponse). Default mirrors v3 + matches
    // what the WebUI's currency picker pre-selects on first boot.
    {prefs::kActCurrencies, FieldKind::kString, false, 0, 0, false, 0,
     "USD,EUR,JPY"},
    // bitaxe — v3 DEFAULT_BITAXE_ENABLED=false,
    // DEFAULT_BITAXE_HOSTNAME="bitaxe1".
    // bitaxePollSec: LAN poll cadence (seconds). Runtime — BitaxeSource::Run()
    // re-reads NVS each tick so a PATCH lands on the next poll without reboot
    // (bd btclock_v4-6hq). Bounds 5..300 keep the AxeOS HTTP server from being
    // hammered while still allowing fast updates during bring-up.
    {prefs::kBitaxeEnabled, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kBitaxeHostname, FieldKind::kString, false, 0, 0, false, 0,
     "bitaxe1"},
    {prefs::kBitaxePollSec, FieldKind::kUint, false, 5, 300, false, 10, {}},
    // v3 DEFAULT_BLOCK_FEE_DECIMALS=true, DEFAULT_BLOCK_FLASH_COLOR=0xE04300.
    {prefs::kBlockFeeDec, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kBlockFlashColor,
     FieldKind::kUint,
     false,
     0,
     0xFFFFFFu,
     false,
     0xE04300,
     {}},
    // custom endpoint — v3 DEFAULT_CUSTOM_ENDPOINT="ws-staging.btclock.dev",
    // DEFAULT_CUSTOM_ENDPOINT_DISABLE_SSL=false.
    {prefs::kCeDisableSSL, FieldKind::kBool, true, 0, 0, false, 0, {}},
    {prefs::kCeEndpoint, FieldKind::kString, true, 0, 0, false, 0,
     "ws-staging.btclock.dev"},
    // v3 DEFAULT_DATA_SOURCE=0 (BTCLOCK_SOURCE).
    {prefs::kDataSource, FieldKind::kUChar, true, 0, 3, false, 0, {}},
    // digitFontPx: pixel height for the big digit glyphs on data screens.
    // Runtime — ScreenManager::Render reads this each frame and pushes it
    // through SetGlobalDigitPx; on_settings_patched marks the screen dirty
    // so the next paint repaints with the new size. Bounds 80..220 leave
    // horizontal headroom for Antonio's widest digit ink within the 122 px
    // short axis (at ~0.55 * px ink/height, 220 → ~121 px). Default 180
    // matches the historical kDigitPx baseline so a fresh install paints
    // identically to pre-feature behaviour.
    {prefs::kDigitFontPx, FieldKind::kUint, false, 80, 220, false, 180, {}},
    // v3 DEFAULT_DISABLE_LEDS=false.
    {prefs::kDisableLeds, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // DND — schedule defaults 22:00-07:00 (the WebUI's pre-fill).
    // Schema carries them so BuildGetResponse, the boot read, and the
    // PATCH handler all derive from one source.
    {prefs::kDndEnabled, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kDndEndHour, FieldKind::kUint, false, 0, 23, false, 7, {}},
    {prefs::kDndEndMin, FieldKind::kUint, false, 0, 59, false, 0, {}},
    {prefs::kDndStartHour, FieldKind::kUint, false, 0, 23, false, 22, {}},
    {prefs::kDndStartMin, FieldKind::kUint, false, 0, 59, false, 0, {}},
    {prefs::kDndTimeEnabled, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // v3 DEFAULT_ENABLE_DEBUG_LOG=false.
    {prefs::kEnableDebugLog, FieldKind::kBool, true, 0, 0, false, 0, {}},
    // frontlight — v3 DEFAULT_FL_ALWAYS_ON=true, DEFAULT_DISABLE_FL=false,
    // DEFAULT_FL_EFFECT_DELAY=15, DEFAULT_FL_FLASH_ON_UPDATE=true,
    // DEFAULT_FL_FLASH_ON_ZAP=true, DEFAULT_FL_MAX_BRIGHTNESS=2048,
    // DEFAULT_FL_OFF_WHEN_DARK=true.
    {prefs::kFlAlwaysOn, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kFlDisable, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kFlEffectDelay, FieldKind::kUint, false, 0, 1000, false, 15, {}},
    {prefs::kFlFlashOnUpd, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // flFlashOnZap: runtime-editable (zap-receipt LED pulse gate) —
    // old firmware exposed it through the generic bool loop.
    {prefs::kFlFlashOnZap, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kFlMaxBrightness,
     FieldKind::kUint,
     false,
     0,
     65535,
     false,
     2048,
     {}},
    {prefs::kFlOffWhenDark, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // v3 DEFAULT_FONT_NAME="antonio", DEFAULT_MINUTES_FULL_REFRESH=60.
    // gitReleaseUrl points at the v4 Forgejo release feed; OtaManager
    // walks `assets[]` and matches `btclock_<variant>_ota.bin` (+
    // sibling `.sha256`) per the naming scheme published by
    // .forgejo/workflows/release.yaml.
    {prefs::kFontName, FieldKind::kString, false, 0, 0, false, 0, "antonio"},
    {prefs::kFullRefreshMin,
     FieldKind::kUint,
     false,
     0,
     24 * 60,
     false,
     60,
     {}},
    {prefs::kGitReleaseUrl, FieldKind::kString, false, 0, 0, false, 0,
     "https://git.btclock.dev/api/v1/repos/btclock/btclock_v4/releases/latest"},
    // gmtOffset: removed from the schema on 2026-04-24 (bd btclock_v4-9rx).
    // v4 uses POSIX TZ strings via setenv("TZ", ...) + tzset(); the old
    // gmtOffset pref was never read back. Legacy NVS entries stay
    // untouched — prefs::kGmtOffset still exists for any reader that
    // needs to migrate, but GET/PATCH no longer surface it.
    // hideLeadZero — clock screen drops the leading zero on single-digit
    // hours ("07:00" → "7:00"). Runtime: ScreenManager reads the pref on
    // every Render and the on_settings_patched hook calls MarkDirty, so a
    // PATCH repaints the next frame without a reboot. v4-only (no v3
    // parallel). Default false preserves legacy HH:MM output.
    {prefs::kHideLeadZero, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // v3 DEFAULT_HOSTNAME_PREFIX="btclock", DEFAULT_HTTP_AUTH_ENABLED=false,
    // DEFAULT_HTTP_AUTH_USERNAME="btclock". httpAuthPass default is "" in
    // the GET emitter because v3 explicitly never shipped the raw password
    // — httpAuthPassSet reports presence instead.
    // hostnamePrefix: runtime — PATCH fires on_mdns_changed in the
    // control server, which calls ReinitMdns to tear down the existing
    // advert (mdns_free) and re-publish under the freshly-computed
    // "<prefix>-<mac>.local" hostname. DHCP keeps the previous lease
    // until the next renewal, so the old name lingers there for ~1 h —
    // mdns is the only path that updates immediately.
    {prefs::kHostnamePrefix, FieldKind::kString, false, 0, 0, false, 0,
     "btclock"},
    {prefs::kHttpAuthEnabled, FieldKind::kBool, true, 0, 0, false, 0, {}},
    {prefs::kHttpAuthPass, FieldKind::kString, true, 0, 0, false, 0, {}},
    {prefs::kHttpAuthUser, FieldKind::kString, true, 0, 0, false, 0, "btclock"},
    // v3 DEFAULT_INVERSE_BUTTONS=false.
    {prefs::kInverseButtons, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // invertedColor: runtime — the EPD driver toggles a global polarity
    // flag (epd::SetGlobalInverted) and main's on_inverted_color_changed
    // hook marks the screen dirty so the next paint repaints with the
    // new polarity. No reboot required. Default is handled in
    // BuildGetResponse (device-dependent: true == white-on-black).
    {prefs::kInvertedColor, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // v3 DEFAULT_LED_BRIGHTNESS=128, DEFAULT_LED_FLASH_ON_ZAP=true,
    // DEFAULT_LED_TEST_ON_POWER=true. ledFlashOnUpd default raised to
    // true in v4 to match flFlashOnUpd — both indicators of a fresh
    // data update should default to the same on-state.
    {prefs::kLedBrightness, FieldKind::kUint, false, 0, 255, false, 128, {}},
    {prefs::kLedFlashOnUpd, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // ledFlashOnZap: runtime — LED pulse gate checked per zap receipt.
    {prefs::kLedFlashOnZap, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kLedTestOnPower, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // v3 DEFAULT_LOCAL_POOL_ENDPOINT="umbrel.local:2019",
    // DEFAULT_LUX_LIGHT_TOGGLE=128.
    {prefs::kLocalPoolHost, FieldKind::kString, true, 0, 0, false, 0,
     "umbrel.local:2019"},
    {prefs::kLuxLightToggle, FieldKind::kUint, false, 0, 65535, false, 128, {}},
    // v3 DEFAULT_MCAP_BIG_CHAR=true, DEFAULT_MDNS_ENABLED=true,
    // DEFAULT_MEMPOOL_INSTANCE="mempool.space", DEFAULT_MEMPOOL_SECURE=true,
    // DEFAULT_SECONDS_BETWEEN_PRICE_UPDATE=30.
    {prefs::kMcapBigChar, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // mdnsEnabled: runtime — PATCH fires on_mdns_changed which either
    // re-runs the advertisement (true) or tears it down via mdns_free
    // (false). Default true matches v3 DEFAULT_MDNS_ENABLED.
    {prefs::kMdnsEnabled, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kMempoolInstance, FieldKind::kString, true, 0, 0, false, 0,
     "mempool.space"},
    {prefs::kMempoolSecure, FieldKind::kBool, true, 0, 0, true, 0, {}},
    {prefs::kMinSecPriceUpd, FieldKind::kUint, false, 1, 3600, false, 30, {}},
    // mining pool — v3 DEFAULT_MINING_POOL_NAME="ocean",
    // DEFAULT_MINING_POOL_STATS_ENABLED=false,
    // DEFAULT_MINING_POOL_USER="38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy".
    // v4 default flipped to "noderunners" alongside `poolGlobalStats=true`
    // so a fresh device has a working data source on first connect (the
    // ocean address is per-user and can't be a sensible global default
    // once the runtime logo fetcher of bd btclock_v4-5yi means we don't
    // ship a vendored ocean bitmap any more).
    {prefs::kMiningPoolName, FieldKind::kString, false, 0, 0, false, 0,
     "noderunners"},
    {prefs::kMiningPoolStats, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kMiningPoolUser, FieldKind::kString, false, 0, 0, false, 0,
     "38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy"},
    // poolWorker: optional secondary identifier scoped under
    // miningPoolUser. NVS-key short form (15-char cap) of what the
    // settings reference docs call the "worker / subaccount slot".
    // Pool semantics — Foundry: subaccount path segment; Braiins/CKPool:
    // worker name (currently unused by their parsers, but surfaced so
    // the WebUI can prompt for it once they consume it). Always plain
    // in GET — the secret bit lives on miningPoolUser via the active
    // pool's user_is_secret() override.
    {prefs::kPoolWorker, FieldKind::kString, false, 0, 0, false, 0, {}},
    // v3 DEFAULT_MOW_MODE=false.
    {prefs::kMowMode, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // nostr — v3 DEFAULT_NOSTR_NPUB=..., DEFAULT_NOSTR_RELAY=relay.primal.net,
    // DEFAULT_ZAP_NOTIFY_ENABLED=false, DEFAULT_ZAP_NOTIFY_PUBKEY=....
    {prefs::kNostrPubKey, FieldKind::kString, true, 0, 0, false, 0,
     "642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288"},
    {prefs::kNostrRelay, FieldKind::kString, true, 0, 0, false, 0,
     "wss://relay.primal.net"},
    {prefs::kNostrZapNotify, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kNostrZapPubkey, FieldKind::kString, false, 0, 0, false, 0,
     "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422"},
    // v3 DEFAULT_OTA_ENABLED=true; otaPass default is empty string in the
    // GET emitter (v3's settings.cpp echoes only `otaPassSet`).
    {prefs::kOtaEnabled, FieldKind::kBool, true, 0, 0, true, 0, {}},
    {prefs::kOtaPass, FieldKind::kString, true, 0, 0, false, 0, {}},
    // v3 DEFAULT_POOL_GLOBAL_STATS=false; v4 flips to true so the new
    // default `noderunners` pool shows the pool-wide aggregate out of
    // the box (no per-user creds required). poolLogosUrl feeds the
    // runtime logo fetcher introduced in bd btclock_v4-5yi.
    {prefs::kPoolGlobalStats, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kPoolLogosUrl, FieldKind::kString, false, 0, 0, false, 0,
     "https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main"},
    // poolPollSec: mining-pool HTTPS poll cadence (seconds). Runtime —
    // PoolDataSource::poll_interval_ms() re-reads NVS each tick so a PATCH
    // lands on the next poll without reboot (bd btclock_v4-gku). Bounds
    // 10..3600 leave room to throttle past the legacy 60 s default for pools
    // that publish per-minute stats already, without overwhelming free
    // public endpoints.
    {prefs::kPoolPollSec, FieldKind::kUint, false, 10, 3600, false, 60, {}},
    // Outbound proxy. Applied at every esp_http_client / esp_websocket_client
    // init point via net_util's ApplyProxyTo* helpers. None of these are
    // boot_only — net_util reads the config per-request, and the
    // on_proxy_changed hook fires kReconnectAll so existing WS sessions
    // re-establish through the new path. proxyType: 0=none, 1=HTTP CONNECT,
    // 2=SOCKS4, 3=SOCKS4a, 4=SOCKS5; bounds clamp to the enum range.
    // proxyPass is suppressed in GET like httpAuthPass — see settings_api.cpp.
    {prefs::kProxyEnabled, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kProxyType, FieldKind::kUChar, false, 0, 4, false, 0, {}},
    {prefs::kProxyHost, FieldKind::kString, false, 0, 0, false, 0, {}},
    {prefs::kProxyPort, FieldKind::kUint, false, 1, 65535, false, 1080, {}},
    {prefs::kProxyUser, FieldKind::kString, false, 0, 0, false, 0, {}},
    {prefs::kProxyPass, FieldKind::kString, false, 0, 0, false, 0, {}},
    {prefs::kProxyBypass, FieldKind::kString, false, 0, 0, false, 0,
     "*.local,192.168.*,10.*,127.0.0.1"},
    // v3 DEFAULT_REFRESH_ON_SCREEN_CHANGE=false,
    // DEFAULT_SCREEN_RESTORE_AFTER_ZAP=true, DEFAULT_SUFFIX_PRICE=false,
    // DEFAULT_SUFFIX_SHARE_DOT=false, DEFAULT_SUPPLY_PERCENT=false.
    // stealFocus default raised to true in v4 so a fresh install jumps
    // to the block-height screen on a new block — matches what most
    // users expect from the "steal on new block" toggle.
    {prefs::kRefrScrnChange, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // satsVariant: index into the 16 glyphs at U+E000..U+E00F in the
    // SatoshiSymbol font. Default 7 = the production glyph that shipped
    // before the variant pref existed. Runtime — the on_sats_variant_changed
    // hook in main pushes the new value into ScreenManager and marks the
    // screen dirty, so the next render paints with the new glyph without
    // a reboot. Bounds 0..15 reject anything outside the valid codepoint
    // range; ClampSatsVariant in main/fonts_app.hpp is the read-side
    // belt-and-braces guard against legacy NVS values.
    {prefs::kSatsVariant, FieldKind::kUint, false, 0, 15, false, 7, {}},
    {prefs::kScrnRestoreZap, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kStealFocus, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kSuffixPrice, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kDecimalShareDot, FieldKind::kBool, false, 0, 0, false, 0, {}},
    {prefs::kSupplyPercent, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // v3 DEFAULT_TZ_STRING="Europe/Amsterdam".
    {prefs::kTzString, FieldKind::kString, false, 0, 0, false, 0,
     "Europe/Amsterdam"},
    // v3 DEFAULT_USE_BLOCK_COUNTDOWN=true, DEFAULT_USE_MSCW_TIME=true,
    // DEFAULT_USE_SATS_SYMBOL=false, DEFAULT_VERTICAL_DESC=true.
    {prefs::kUseBlkCountdown, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kUseMscwTime, FieldKind::kBool, false, 0, 0, true, 0, {}},
    {prefs::kUseSatsSymbol, FieldKind::kBool, false, 0, 0, false, 0, {}},
    // verticalDesc: runtime — affects EPD layout on next render.
    {prefs::kVerticalDesc, FieldKind::kBool, false, 0, 0, true, 0, {}},
    // wifiRebootOutageMinutes: v4-only soft-watchdog (no v3 parallel).
    // Default to 10 minutes — matches the old Arduino main.cpp
    // checkWiFiConnection() 10-minute brute-force reset cadence.
    {prefs::kWifiRebootMin, FieldKind::kUint, false, 0, 120, false, 10, {}},
    // wpTimeout: WiFiManager captive-portal timeout (seconds). Boot-only
    // because the portal only reads it during provisioning bring-up.
    // v3 DEFAULT_WP_TIMEOUT = 15 * 60 seconds.
    {prefs::kWpTimeout, FieldKind::kUint, true, 0, 3600, false, 15 * 60, {}},
}};

// Lookup by key. Returns nullptr if unknown. Linear scan — N ~ 60,
// the table lives in flash, and PATCH calls are rare, so std::find
// + friends would out-weigh themselves.
inline const FieldSpec* FindField(std::string_view key) {
  for (const auto& f : kFields) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

// Schema-default accessors. The single source of truth for every key's
// default value lives in `kFields[i].default_*`; the EmitField loop in
// settings_api.cpp consumes that already, but a handful of special-case
// emit/read paths (boot reads, GET overrides, runtime fetchers in other
// components) used to hardcode their own copy. Use these helpers there
// instead so flipping a default is a one-line schema edit.
inline bool DefaultBoolFor(std::string_view key) {
  const auto* f = FindField(key);
  return (f && f->kind == FieldKind::kBool) ? f->default_bool : false;
}
inline int32_t DefaultIntFor(std::string_view key) {
  const auto* f = FindField(key);
  if (!f) return 0;
  switch (f->kind) {
    case FieldKind::kUint:
    case FieldKind::kInt:
    case FieldKind::kUChar:
      return f->default_int;
    default:
      return 0;
  }
}
inline std::string_view DefaultStringFor(std::string_view key) {
  const auto* f = FindField(key);
  return (f && f->kind == FieldKind::kString) ? f->default_str
                                              : std::string_view{};
}

// Schema-backed prefs readers. Call these instead of
// `prefs.GetBool(key, <hardcoded>)` from any code path that consumes a
// settings-namespace NVS value — they pull the fallback from the
// schema, eliminating the per-call-site default duplication that used
// to drift (most notably invertedColor's three-way split).
//
// Templated on the prefs type so this header stays free of a
// `components/prefs` dependency; the concrete `Prefs` and the test-only
// `MapPrefs` in test_host both satisfy the duck-typed interface.
//
// String overload also folds an empty NVS value back to the schema
// default whenever the schema declares a non-empty default. Covers the
// case where a stale PATCH wrote "" or where the key was briefly
// removed from the schema and re-added (the NVS slot lingers as
// empty). For fields whose schema default is itself empty (httpAuthPass,
// otaPass, poolWorker), the fold is a no-op, so callers that genuinely
// want "" stored stay correct.
template <typename PrefsT>
inline bool ReadBool(PrefsT& p, const char* key) {
  return p.GetBool(key, DefaultBoolFor(key));
}
template <typename PrefsT>
inline uint32_t ReadU32(PrefsT& p, const char* key) {
  return p.GetU32(key, static_cast<uint32_t>(DefaultIntFor(key)));
}
template <typename PrefsT>
inline int32_t ReadI32(PrefsT& p, const char* key) {
  return p.GetI32(key, DefaultIntFor(key));
}
template <typename PrefsT>
inline uint8_t ReadU8(PrefsT& p, const char* key) {
  return p.GetU8(key, static_cast<uint8_t>(DefaultIntFor(key)));
}
template <typename PrefsT>
inline std::string ReadString(PrefsT& p, const char* key) {
  const auto sv = DefaultStringFor(key);
  const std::string fallback(sv);
  std::string out = p.GetString(key, fallback.c_str());
  if (out.empty() && !sv.empty()) out = fallback;
  return out;
}

// Count of boot-only fields — used by the test suite to detect drift
// if someone adds a field without thinking through the classification.
inline constexpr size_t BootOnlyCount() {
  size_t n = 0;
  for (const auto& f : kFields) {
    if (f.boot_only) ++n;
  }
  return n;
}

}  // namespace settings
}  // namespace btclock
