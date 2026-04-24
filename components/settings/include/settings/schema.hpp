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
  kUint,   // stored as NVS u32
  kInt,    // stored as NVS i32 — signed (tx power, gmt offset)
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
//   - WiFi/network identity fields (hostnamePrefix, mdnsEnabled,
//     httpAuthEnabled, httpAuthUser, httpAuthPass, otaPass, otaEnabled)
//     require reboot — they drive services started exactly once in
//     setupWifi() / setupWebserver() / ArduinoOTA.begin().
//   - Data-source fields (dataSource, mempoolInstance, mempoolSecure,
//     ceEndpoint, ceDisableSSL, localPoolHost) reboot because the
//     old firmware tears down the WS client and setupDataSource()
//     only runs at boot (it's legal to call again, but it leaves
//     dangling tasks in practice).
//   - Display driver settings (fontName, invertedColor) reboot because
//     they touch EPD driver state only initialised in setup().
//   - Everything else takes effect live: LED brightness, DND, screen
//     visibility, countdown flags, etc.
//
// Range bounds only where the old firmware enforced them or where a
// bad value crashes hardware (brightness > 8-bit, hour > 23, etc.).
//
// Defaults track btclock_v3_fci/src/lib/system/defaults.hpp so a fresh
// install returns the same values the Arduino firmware did. Where v3
// had no default at all (plain preferences.getXxx(key) with the C++
// zero-init fallback), the spec entry leaves the default_* fields at
// their struct-init zero values. See docs/SETTINGS.md for the audit
// table.
inline constexpr std::array<FieldSpec, 63> kFields = {{
    // bitaxe — v3 DEFAULT_BITAXE_ENABLED=false, DEFAULT_BITAXE_HOSTNAME="bitaxe1".
    {prefs::kBitaxeEnabled,    FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kBitaxeHostname,   FieldKind::kString, false, 0, 0,          false, 0, "bitaxe1"},
    // v3 DEFAULT_BLOCK_FEE_DECIMALS=true, DEFAULT_BLOCK_FLASH_COLOR=0xE04300.
    {prefs::kBlockFeeDec,      FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kBlockFlashColor,  FieldKind::kUint,   false, 0, 0xFFFFFFu,  false, 0xE04300, {}},
    // custom endpoint — v3 DEFAULT_CUSTOM_ENDPOINT="ws-staging.btclock.dev",
    // DEFAULT_CUSTOM_ENDPOINT_DISABLE_SSL=false.
    {prefs::kCeDisableSSL,     FieldKind::kBool,   true,  0, 0,          false, 0, {}},
    {prefs::kCeEndpoint,       FieldKind::kString, true,  0, 0,          false, 0, "ws-staging.btclock.dev"},
    // v3 DEFAULT_DATA_SOURCE=0 (BTCLOCK_SOURCE).
    {prefs::kDataSource,       FieldKind::kUChar,  true,  0, 3,          false, 0, {}},
    // v3 DEFAULT_DISABLE_LEDS=false.
    {prefs::kDisableLeds,      FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // DND — v3 had no documented defaults; the top-level dnd block in the
    // response uses its own explicit defaults (enabled=false, schedule
    // 22:00-07:00) set directly in BuildGetResponse. Leaving schema at
    // false/0 keeps the flat GET mirror consistent with that.
    {prefs::kDndEnabled,       FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kDndTimeEnabled,   FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // v3 DEFAULT_ENABLE_DEBUG_LOG=false.
    {prefs::kEnableDebugLog,   FieldKind::kBool,   true,  0, 0,          false, 0, {}},
    // frontlight — v3 DEFAULT_FL_ALWAYS_ON=true, DEFAULT_DISABLE_FL=false,
    // DEFAULT_FL_EFFECT_DELAY=15, DEFAULT_FL_FLASH_ON_UPDATE=true,
    // DEFAULT_FL_FLASH_ON_ZAP=true, DEFAULT_FL_MAX_BRIGHTNESS=2048,
    // DEFAULT_FL_OFF_WHEN_DARK=true.
    {prefs::kFlAlwaysOn,       FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kFlDisable,        FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kFlEffectDelay,    FieldKind::kUint,   false, 0, 1000,       false, 15, {}},
    {prefs::kFlFlashOnUpd,     FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    // flFlashOnZap: runtime-editable (zap-receipt LED pulse gate) —
    // old firmware exposed it through the generic bool loop.
    {prefs::kFlFlashOnZap,     FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kFlMaxBrightness,  FieldKind::kUint,   false, 0, 65535,      false, 2048, {}},
    {prefs::kFlOffWhenDark,    FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    // v3 DEFAULT_FONT_NAME="antonio", DEFAULT_MINUTES_FULL_REFRESH=60,
    // DEFAULT_GIT_RELEASE_URL=the v3 release feed.
    {prefs::kFontName,         FieldKind::kString, true,  0, 0,          false, 0, "antonio"},
    {prefs::kFullRefreshMin,   FieldKind::kUint,   false, 0, 24 * 60,    false, 60, {}},
    {prefs::kGitReleaseUrl,    FieldKind::kString, false, 0, 0,          false, 0,
     "https://git.btclock.dev/api/v1/repos/btclock/btclock_v3/releases/latest"},
    // gmtOffset: removed from the schema on 2026-04-24 (bd btclock_v4-9rx).
    // v4 uses POSIX TZ strings via setenv("TZ", ...) + tzset(); the old
    // gmtOffset pref was never read back. Legacy NVS entries stay
    // untouched — prefs::kGmtOffset still exists for any reader that
    // needs to migrate, but GET/PATCH no longer surface it.
    // v3 DEFAULT_HOSTNAME_PREFIX="btclock", DEFAULT_HTTP_AUTH_ENABLED=false,
    // DEFAULT_HTTP_AUTH_USERNAME="btclock". httpAuthPass default is "" in
    // the GET emitter because v3 explicitly never shipped the raw password
    // — httpAuthPassSet reports presence instead.
    {prefs::kHostnamePrefix,   FieldKind::kString, true,  0, 0,          false, 0, "btclock"},
    {prefs::kHttpAuthEnabled,  FieldKind::kBool,   true,  0, 0,          false, 0, {}},
    {prefs::kHttpAuthPass,     FieldKind::kString, true,  0, 0,          false, 0, {}},
    {prefs::kHttpAuthUser,     FieldKind::kString, true,  0, 0,          false, 0, "btclock"},
    // v3 DEFAULT_INVERSE_BUTTONS=false.
    {prefs::kInverseButtons,   FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // invertedColor: runtime — the EPD driver toggles a global polarity
    // flag (EpdSetGlobalInverted) and main's on_inverted_color_changed
    // hook marks the screen dirty so the next paint repaints with the
    // new polarity. No reboot required. Default is handled in
    // BuildGetResponse (device-dependent: true == white-on-black).
    {prefs::kInvertedColor,    FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // v3 DEFAULT_LED_BRIGHTNESS=128, DEFAULT_LED_FLASH_ON_UPD=false,
    // DEFAULT_LED_FLASH_ON_ZAP=true, DEFAULT_LED_TEST_ON_POWER=true.
    {prefs::kLedBrightness,    FieldKind::kUint,   false, 0, 255,        false, 128, {}},
    {prefs::kLedFlashOnUpd,    FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // ledFlashOnZap: runtime — LED pulse gate checked per zap receipt.
    {prefs::kLedFlashOnZap,    FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kLedTestOnPower,   FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    // v3 DEFAULT_LOCAL_POOL_ENDPOINT="umbrel.local:2019",
    // DEFAULT_LUX_LIGHT_TOGGLE=128.
    {prefs::kLocalPoolHost,    FieldKind::kString, true,  0, 0,          false, 0, "umbrel.local:2019"},
    {prefs::kLuxLightToggle,   FieldKind::kUint,   false, 0, 65535,      false, 128, {}},
    // v3 DEFAULT_MCAP_BIG_CHAR=true, DEFAULT_MDNS_ENABLED=true,
    // DEFAULT_MEMPOOL_INSTANCE="mempool.space", DEFAULT_MEMPOOL_SECURE=true,
    // DEFAULT_SECONDS_BETWEEN_PRICE_UPDATE=30.
    {prefs::kMcapBigChar,      FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kMdnsEnabled,      FieldKind::kBool,   true,  0, 0,          true,  0, {}},
    {prefs::kMempoolInstance,  FieldKind::kString, true,  0, 0,          false, 0, "mempool.space"},
    {prefs::kMempoolSecure,    FieldKind::kBool,   true,  0, 0,          true,  0, {}},
    {prefs::kMinSecPriceUpd,   FieldKind::kUint,   false, 1, 3600,       false, 30, {}},
    // mining pool — v3 DEFAULT_MINING_POOL_NAME="ocean",
    // DEFAULT_MINING_POOL_STATS_ENABLED=false,
    // DEFAULT_MINING_POOL_USER="38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy".
    {prefs::kMiningPoolName,   FieldKind::kString, false, 0, 0,          false, 0, "ocean"},
    {prefs::kMiningPoolStats,  FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kMiningPoolUser,   FieldKind::kString, false, 0, 0,          false, 0,
     "38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy"},
    // v3 DEFAULT_MOW_MODE=false.
    {prefs::kMowMode,          FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // nostr — v3 DEFAULT_NOSTR_NPUB=..., DEFAULT_NOSTR_RELAY=relay.primal.net,
    // DEFAULT_ZAP_NOTIFY_ENABLED=false, DEFAULT_ZAP_NOTIFY_PUBKEY=....
    {prefs::kNostrPubKey,      FieldKind::kString, true,  0, 0,          false, 0,
     "642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288"},
    {prefs::kNostrRelay,       FieldKind::kString, true,  0, 0,          false, 0, "wss://relay.primal.net"},
    {prefs::kNostrZapNotify,   FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kNostrZapPubkey,   FieldKind::kString, false, 0, 0,          false, 0,
     "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422"},
    // v3 DEFAULT_OTA_ENABLED=true; otaPass default is empty string in the
    // GET emitter (v3's settings.cpp echoes only `otaPassSet`).
    {prefs::kOtaEnabled,       FieldKind::kBool,   true,  0, 0,          true,  0, {}},
    {prefs::kOtaPass,          FieldKind::kString, true,  0, 0,          false, 0, {}},
    // v3 DEFAULT_POOL_GLOBAL_STATS=false, DEFAULT_MINING_POOL_LOGOS_URL=
    // the shared mining-pool-logos repo on git.btclock.dev.
    {prefs::kPoolGlobalStats,  FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kPoolLogosUrl,     FieldKind::kString, false, 0, 0,          false, 0,
     "https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main"},
    // v3 DEFAULT_REFRESH_ON_SCREEN_CHANGE=false,
    // DEFAULT_SCREEN_RESTORE_AFTER_ZAP=true, DEFAULT_STEAL_FOCUS=false,
    // DEFAULT_SUFFIX_PRICE=false, DEFAULT_SUFFIX_SHARE_DOT=false,
    // DEFAULT_SUPPLY_PERCENT=false.
    {prefs::kRefrScrnChange,   FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kScrnRestoreZap,   FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kStealFocus,       FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kSuffixPrice,      FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kSuffixShareDot,   FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    {prefs::kSupplyPercent,    FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // v3 DEFAULT_TZ_STRING="Europe/Amsterdam".
    {prefs::kTzString,         FieldKind::kString, false, 0, 0,          false, 0, "Europe/Amsterdam"},
    // v3 DEFAULT_USE_BLOCK_COUNTDOWN=true, DEFAULT_USE_MSCW_TIME=true,
    // DEFAULT_USE_SATS_SYMBOL=false, DEFAULT_VERTICAL_DESC=true.
    {prefs::kUseBlkCountdown,  FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kUseMscwTime,      FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    {prefs::kUseSatsSymbol,    FieldKind::kBool,   false, 0, 0,          false, 0, {}},
    // verticalDesc: runtime — affects EPD layout on next render.
    {prefs::kVerticalDesc,     FieldKind::kBool,   false, 0, 0,          true,  0, {}},
    // wifiRebootOutageMinutes: v4-only soft-watchdog (no v3 parallel).
    // Default to 10 minutes — matches the old Arduino main.cpp
    // checkWiFiConnection() 10-minute brute-force reset cadence.
    {prefs::kWifiRebootMin,    FieldKind::kUint,   false, 0, 120,        false, 10, {}},
    // wpTimeout: WiFiManager captive-portal timeout (seconds). Boot-only
    // because the portal only reads it during provisioning bring-up.
    // v3 DEFAULT_WP_TIMEOUT = 15 * 60 seconds.
    {prefs::kWpTimeout,        FieldKind::kUint,   true,  0, 3600,       false, 15 * 60, {}},
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
