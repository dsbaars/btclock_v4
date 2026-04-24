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
inline constexpr std::array<FieldSpec, 64> kFields = {{
    {prefs::kBitaxeEnabled,    FieldKind::kBool,   false, 0, 0},
    {prefs::kBitaxeHostname,   FieldKind::kString, false, 0, 0},
    {prefs::kBlockFeeDec,      FieldKind::kBool,   false, 0, 0},
    {prefs::kBlockFlashColor,  FieldKind::kUint,   false, 0, 0xFFFFFFu},
    {prefs::kCeDisableSSL,     FieldKind::kBool,   true,  0, 0},
    {prefs::kCeEndpoint,       FieldKind::kString, true,  0, 0},
    {prefs::kDataSource,       FieldKind::kUChar,  true,  0, 3},
    {prefs::kDisableLeds,      FieldKind::kBool,   false, 0, 0},
    {prefs::kDndEnabled,       FieldKind::kBool,   false, 0, 0},
    {prefs::kDndTimeEnabled,   FieldKind::kBool,   false, 0, 0},
    {prefs::kEnableDebugLog,   FieldKind::kBool,   true,  0, 0},
    {prefs::kFlAlwaysOn,       FieldKind::kBool,   false, 0, 0},
    {prefs::kFlDisable,        FieldKind::kBool,   false, 0, 0},
    {prefs::kFlEffectDelay,    FieldKind::kUint,   false, 0, 1000},
    {prefs::kFlFlashOnUpd,     FieldKind::kBool,   false, 0, 0},
    // flFlashOnZap: runtime-editable (zap-receipt LED pulse gate) —
    // old firmware exposed it through the generic bool loop.
    {prefs::kFlFlashOnZap,     FieldKind::kBool,   false, 0, 0},
    {prefs::kFlMaxBrightness,  FieldKind::kUint,   false, 0, 65535},
    {prefs::kFlOffWhenDark,    FieldKind::kBool,   false, 0, 0},
    {prefs::kFontName,         FieldKind::kString, true,  0, 0},
    {prefs::kFullRefreshMin,   FieldKind::kUint,   false, 0, 24 * 60},
    {prefs::kGitReleaseUrl,    FieldKind::kString, false, 0, 0},
    // gmtOffset: boot-only — only read by NTP init (sntp_set_time_sync*)
    // and not re-applied live. Minutes-based input (`tzOffset`) is
    // handled separately in ApplyPatch; direct PATCH of gmtOffset is
    // accepted as a seconds-based integer for parity with the raw pref.
    {prefs::kGmtOffset,        FieldKind::kInt,    true,  0, 0},
    {prefs::kHostnamePrefix,   FieldKind::kString, true,  0, 0},
    {prefs::kHttpAuthEnabled,  FieldKind::kBool,   true,  0, 0},
    {prefs::kHttpAuthPass,     FieldKind::kString, true,  0, 0},
    {prefs::kHttpAuthUser,     FieldKind::kString, true,  0, 0},
    {prefs::kInverseButtons,   FieldKind::kBool,   false, 0, 0},
    // invertedColor: runtime — the EPD driver toggles a global polarity
    // flag (EpdSetGlobalInverted) and main's on_inverted_color_changed
    // hook marks the screen dirty so the next paint repaints with the
    // new polarity. No reboot required.
    {prefs::kInvertedColor,    FieldKind::kBool,   false, 0, 0},
    {prefs::kLedBrightness,    FieldKind::kUint,   false, 0, 255},
    {prefs::kLedFlashOnUpd,    FieldKind::kBool,   false, 0, 0},
    // ledFlashOnZap: runtime — LED pulse gate checked per zap receipt.
    {prefs::kLedFlashOnZap,    FieldKind::kBool,   false, 0, 0},
    {prefs::kLedTestOnPower,   FieldKind::kBool,   false, 0, 0},
    {prefs::kLocalPoolHost,    FieldKind::kString, true,  0, 0},
    {prefs::kLuxLightToggle,   FieldKind::kUint,   false, 0, 65535},
    {prefs::kMcapBigChar,      FieldKind::kBool,   false, 0, 0},
    {prefs::kMdnsEnabled,      FieldKind::kBool,   true,  0, 0},
    {prefs::kMempoolInstance,  FieldKind::kString, true,  0, 0},
    {prefs::kMempoolSecure,    FieldKind::kBool,   true,  0, 0},
    {prefs::kMinSecPriceUpd,   FieldKind::kUint,   false, 1, 3600},
    {prefs::kMiningPoolName,   FieldKind::kString, false, 0, 0},
    {prefs::kMiningPoolStats,  FieldKind::kBool,   false, 0, 0},
    {prefs::kMiningPoolUser,   FieldKind::kString, false, 0, 0},
    {prefs::kMowMode,          FieldKind::kBool,   false, 0, 0},
    {prefs::kNostrPubKey,      FieldKind::kString, true,  0, 0},
    {prefs::kNostrRelay,       FieldKind::kString, true,  0, 0},
    {prefs::kNostrZapNotify,   FieldKind::kBool,   false, 0, 0},
    {prefs::kNostrZapPubkey,   FieldKind::kString, false, 0, 0},
    {prefs::kOtaEnabled,       FieldKind::kBool,   true,  0, 0},
    {prefs::kOtaPass,          FieldKind::kString, true,  0, 0},
    {prefs::kPoolGlobalStats,  FieldKind::kBool,   false, 0, 0},
    {prefs::kPoolLogosUrl,     FieldKind::kString, false, 0, 0},
    {prefs::kRefrScrnChange,   FieldKind::kBool,   false, 0, 0},
    {prefs::kScrnRestoreZap,   FieldKind::kBool,   false, 0, 0},
    {prefs::kStealFocus,       FieldKind::kBool,   false, 0, 0},
    {prefs::kSuffixPrice,      FieldKind::kBool,   false, 0, 0},
    {prefs::kSuffixShareDot,   FieldKind::kBool,   false, 0, 0},
    {prefs::kSupplyPercent,    FieldKind::kBool,   false, 0, 0},
    {prefs::kTzString,         FieldKind::kString, false, 0, 0},
    {prefs::kUseBlkCountdown,  FieldKind::kBool,   false, 0, 0},
    {prefs::kUseMscwTime,      FieldKind::kBool,   false, 0, 0},
    {prefs::kUseSatsSymbol,    FieldKind::kBool,   false, 0, 0},
    // verticalDesc: runtime — affects EPD layout on next render.
    {prefs::kVerticalDesc,     FieldKind::kBool,   false, 0, 0},
    // wifiRebootOutageMinutes: soft-watchdog that reboots after N
    // minutes of continuous STA disconnect. 0 disables. Runtime-
    // applied: wifi_guard's tick re-reads the member on every fire,
    // so a live PATCH doesn't require reboot.
    {prefs::kWifiRebootMin,    FieldKind::kUint,   false, 0, 120},
    // wpTimeout: WiFiManager captive-portal timeout (seconds). Boot-only
    // because the portal only reads it during provisioning bring-up.
    {prefs::kWpTimeout,        FieldKind::kUint,   true,  0, 3600},
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
