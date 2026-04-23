// Central inventory of every NVS (Preferences) key the settings
// subsystem touches. Ported from src/lib/system/pref_keys.hpp — the
// string literals are the public API contract with the WebUI and the
// on-flash NVS namespace, so they must match the production firmware
// byte-for-byte.
//
// Two constraints shape this list:
//   1. ESP-IDF's underlying NVS store caps keys at 15 characters (see
//      nvs.h NVS_KEY_NAME_MAX_SIZE). None of the constants below may
//      exceed that limit; the static_assert block at the bottom of
//      this file enforces it at build time.
//   2. The WebUI sends and receives the raw key names in /api/settings
//      payloads, so the *values* of these constants are effectively
//      part of the firmware's public API contract. Do not rename a
//      key without a migration path for existing installs.
//
// Conventions:
//   - Every constant is namespaced under `btclock::prefs::` and kept
//     as an `inline constexpr const char*` so it can be used in
//     constant expressions without ODR issues.
//   - Deprecated keys that must still be read (for migration) are
//     tagged with a trailing `_Legacy` suffix and an adjacent comment
//     pointing to the replacement.

#pragma once

#include <cstring>

namespace btclock {
namespace prefs {

inline constexpr const char* kActCurrencies    = "actCurrencies";
inline constexpr const char* kBgColor          = "bgColor";
inline constexpr const char* kBitaxeEnabled    = "bitaxeEnabled";
inline constexpr const char* kBitaxeHostname   = "bitaxeHostname";
inline constexpr const char* kBlockFeeDec      = "blockFeeDec";
inline constexpr const char* kBlockFlashColor  = "blockFlashColor";
inline constexpr const char* kBlockHeight      = "blockHeight";
inline constexpr const char* kCeDisableSSL     = "ceDisableSSL";
inline constexpr const char* kCeEndpoint       = "ceEndpoint";
inline constexpr const char* kCurrentScreen    = "currentScreen";
inline constexpr const char* kDataSource       = "dataSource";
inline constexpr const char* kDisableLeds      = "disableLeds";
inline constexpr const char* kDndEnabled       = "dndEnabled";
inline constexpr const char* kDndEndHour       = "dndEndHour";
inline constexpr const char* kDndEndMin        = "dndEndMin";
inline constexpr const char* kDndStartHour     = "dndStartHour";
inline constexpr const char* kDndStartMin      = "dndStartMin";
inline constexpr const char* kDndTimeEnabled   = "dndTimeEnabled";
inline constexpr const char* kEnableDebugLog   = "enableDebugLog";
inline constexpr const char* kFgColor          = "fgColor";
inline constexpr const char* kFlAlwaysOn       = "flAlwaysOn";
inline constexpr const char* kFlDisable        = "flDisable";
inline constexpr const char* kFlEffectDelay    = "flEffectDelay";
inline constexpr const char* kFlFlashOnUpd     = "flFlashOnUpd";
inline constexpr const char* kFlFlashOnZap     = "flFlashOnZap";
inline constexpr const char* kFlMaxBrightness  = "flMaxBrightness";
inline constexpr const char* kFlOffWhenDark    = "flOffWhenDark";
inline constexpr const char* kFontName         = "fontName";
inline constexpr const char* kFullRefreshMin   = "fullRefreshMin";
inline constexpr const char* kGitReleaseUrl    = "gitReleaseUrl";
inline constexpr const char* kGmtOffset        = "gmtOffset";
inline constexpr const char* kHostnamePrefix   = "hostnamePrefix";
inline constexpr const char* kHttpAuthEnabled  = "httpAuthEnabled";
inline constexpr const char* kHttpAuthPass     = "httpAuthPass";
inline constexpr const char* kHttpAuthUser     = "httpAuthUser";
inline constexpr const char* kInverseButtons   = "inverseButtons";
inline constexpr const char* kInvertedColor    = "invertedColor";
inline constexpr const char* kLedBrightness    = "ledBrightness";
inline constexpr const char* kLedFlashOnUpd    = "ledFlashOnUpd";
inline constexpr const char* kLedFlashOnZap    = "ledFlashOnZap";
inline constexpr const char* kLedTestOnPower   = "ledTestOnPower";
inline constexpr const char* kLocalPoolHost    = "localPoolHost";
inline constexpr const char* kLuxLightToggle   = "luxLightToggle";
inline constexpr const char* kMcapBigChar      = "mcapBigChar";
inline constexpr const char* kMdnsEnabled      = "mdnsEnabled";
inline constexpr const char* kMempoolInstance  = "mempoolInstance";
inline constexpr const char* kMempoolSecure    = "mempoolSecure";
inline constexpr const char* kMinSecPriceUpd   = "minSecPriceUpd";
inline constexpr const char* kMiningPoolName   = "miningPoolName";
inline constexpr const char* kMiningPoolStats  = "miningPoolStats";
inline constexpr const char* kMiningPoolUser   = "miningPoolUser";
inline constexpr const char* kMowMode          = "mowMode";
inline constexpr const char* kNostrPubKey      = "nostrPubKey";
inline constexpr const char* kNostrRelay       = "nostrRelay";
inline constexpr const char* kNostrZapNotify   = "nostrZapNotify";
inline constexpr const char* kNostrZapPubkey   = "nostrZapPubkey";
inline constexpr const char* kOtaEnabled       = "otaEnabled";
inline constexpr const char* kOtaPass          = "otaPass";
inline constexpr const char* kPoolGlobalStats  = "poolGlobalStats";
inline constexpr const char* kPoolLogosUrl     = "poolLogosUrl";
inline constexpr const char* kRefrScrnChange   = "refrScrnChange";
inline constexpr const char* kScreenOrder      = "screenOrder";
inline constexpr const char* kScrnRestoreZap   = "scrnRestoreZap";
inline constexpr const char* kStealFocus       = "stealFocus";
inline constexpr const char* kSuffixPrice      = "suffixPrice";
inline constexpr const char* kSuffixShareDot   = "suffixShareDot";
inline constexpr const char* kSupplyPercent    = "supplyPercent";
inline constexpr const char* kTimerActive      = "timerActive";
inline constexpr const char* kTimerSeconds     = "timerSeconds";
inline constexpr const char* kTxPower          = "txPower";
inline constexpr const char* kTzString         = "tzString";
inline constexpr const char* kUseBlkCountdown  = "useBlkCountdown";
inline constexpr const char* kUseMscwTime      = "useMscwTime";
inline constexpr const char* kUseSatsSymbol    = "useSatsSymbol";
inline constexpr const char* kVerticalDesc     = "verticalDesc";
inline constexpr const char* kWifiConfigured   = "wifiConfigured";
inline constexpr const char* kWpTimeout        = "wpTimeout";

// NVS namespace that holds every key listed above. One namespace keeps
// settings migration atomic — a reset blows the whole thing away, and
// a backup can be taken with nvs_entry_find("nvs", kSettingsNs, ...).
inline constexpr const char* kSettingsNs = "settings";

// Compile-time guard: NVS keys must be <= 15 chars. Enforced once here
// so a future rename can't silently truncate on-flash.
constexpr bool FitsNvsLimit(const char* s) {
  // strlen isn't constexpr before C++23; walk manually.
  std::size_t n = 0;
  while (s[n] != '\0') ++n;
  return n <= 15;
}

#define BTCLOCK_PREF_KEY_ASSERT(name) \
  static_assert(FitsNvsLimit(name), #name " exceeds 15-char NVS limit")

BTCLOCK_PREF_KEY_ASSERT(kActCurrencies);
BTCLOCK_PREF_KEY_ASSERT(kBgColor);
BTCLOCK_PREF_KEY_ASSERT(kBitaxeEnabled);
BTCLOCK_PREF_KEY_ASSERT(kBitaxeHostname);
BTCLOCK_PREF_KEY_ASSERT(kBlockFeeDec);
BTCLOCK_PREF_KEY_ASSERT(kBlockFlashColor);
BTCLOCK_PREF_KEY_ASSERT(kBlockHeight);
BTCLOCK_PREF_KEY_ASSERT(kCeDisableSSL);
BTCLOCK_PREF_KEY_ASSERT(kCeEndpoint);
BTCLOCK_PREF_KEY_ASSERT(kCurrentScreen);
BTCLOCK_PREF_KEY_ASSERT(kDataSource);
BTCLOCK_PREF_KEY_ASSERT(kDisableLeds);
BTCLOCK_PREF_KEY_ASSERT(kDndEnabled);
BTCLOCK_PREF_KEY_ASSERT(kDndEndHour);
BTCLOCK_PREF_KEY_ASSERT(kDndEndMin);
BTCLOCK_PREF_KEY_ASSERT(kDndStartHour);
BTCLOCK_PREF_KEY_ASSERT(kDndStartMin);
BTCLOCK_PREF_KEY_ASSERT(kDndTimeEnabled);
BTCLOCK_PREF_KEY_ASSERT(kEnableDebugLog);
BTCLOCK_PREF_KEY_ASSERT(kFgColor);
BTCLOCK_PREF_KEY_ASSERT(kFlAlwaysOn);
BTCLOCK_PREF_KEY_ASSERT(kFlDisable);
BTCLOCK_PREF_KEY_ASSERT(kFlEffectDelay);
BTCLOCK_PREF_KEY_ASSERT(kFlFlashOnUpd);
BTCLOCK_PREF_KEY_ASSERT(kFlFlashOnZap);
BTCLOCK_PREF_KEY_ASSERT(kFlMaxBrightness);
BTCLOCK_PREF_KEY_ASSERT(kFlOffWhenDark);
BTCLOCK_PREF_KEY_ASSERT(kFontName);
BTCLOCK_PREF_KEY_ASSERT(kFullRefreshMin);
BTCLOCK_PREF_KEY_ASSERT(kGitReleaseUrl);
BTCLOCK_PREF_KEY_ASSERT(kGmtOffset);
BTCLOCK_PREF_KEY_ASSERT(kHostnamePrefix);
BTCLOCK_PREF_KEY_ASSERT(kHttpAuthEnabled);
BTCLOCK_PREF_KEY_ASSERT(kHttpAuthPass);
BTCLOCK_PREF_KEY_ASSERT(kHttpAuthUser);
BTCLOCK_PREF_KEY_ASSERT(kInverseButtons);
BTCLOCK_PREF_KEY_ASSERT(kInvertedColor);
BTCLOCK_PREF_KEY_ASSERT(kLedBrightness);
BTCLOCK_PREF_KEY_ASSERT(kLedFlashOnUpd);
BTCLOCK_PREF_KEY_ASSERT(kLedFlashOnZap);
BTCLOCK_PREF_KEY_ASSERT(kLedTestOnPower);
BTCLOCK_PREF_KEY_ASSERT(kLocalPoolHost);
BTCLOCK_PREF_KEY_ASSERT(kLuxLightToggle);
BTCLOCK_PREF_KEY_ASSERT(kMcapBigChar);
BTCLOCK_PREF_KEY_ASSERT(kMdnsEnabled);
BTCLOCK_PREF_KEY_ASSERT(kMempoolInstance);
BTCLOCK_PREF_KEY_ASSERT(kMempoolSecure);
BTCLOCK_PREF_KEY_ASSERT(kMinSecPriceUpd);
BTCLOCK_PREF_KEY_ASSERT(kMiningPoolName);
BTCLOCK_PREF_KEY_ASSERT(kMiningPoolStats);
BTCLOCK_PREF_KEY_ASSERT(kMiningPoolUser);
BTCLOCK_PREF_KEY_ASSERT(kMowMode);
BTCLOCK_PREF_KEY_ASSERT(kNostrPubKey);
BTCLOCK_PREF_KEY_ASSERT(kNostrRelay);
BTCLOCK_PREF_KEY_ASSERT(kNostrZapNotify);
BTCLOCK_PREF_KEY_ASSERT(kNostrZapPubkey);
BTCLOCK_PREF_KEY_ASSERT(kOtaEnabled);
BTCLOCK_PREF_KEY_ASSERT(kOtaPass);
BTCLOCK_PREF_KEY_ASSERT(kPoolGlobalStats);
BTCLOCK_PREF_KEY_ASSERT(kPoolLogosUrl);
BTCLOCK_PREF_KEY_ASSERT(kRefrScrnChange);
BTCLOCK_PREF_KEY_ASSERT(kScreenOrder);
BTCLOCK_PREF_KEY_ASSERT(kScrnRestoreZap);
BTCLOCK_PREF_KEY_ASSERT(kStealFocus);
BTCLOCK_PREF_KEY_ASSERT(kSuffixPrice);
BTCLOCK_PREF_KEY_ASSERT(kSuffixShareDot);
BTCLOCK_PREF_KEY_ASSERT(kSupplyPercent);
BTCLOCK_PREF_KEY_ASSERT(kTimerActive);
BTCLOCK_PREF_KEY_ASSERT(kTimerSeconds);
BTCLOCK_PREF_KEY_ASSERT(kTxPower);
BTCLOCK_PREF_KEY_ASSERT(kTzString);
BTCLOCK_PREF_KEY_ASSERT(kUseBlkCountdown);
BTCLOCK_PREF_KEY_ASSERT(kUseMscwTime);
BTCLOCK_PREF_KEY_ASSERT(kUseSatsSymbol);
BTCLOCK_PREF_KEY_ASSERT(kVerticalDesc);
BTCLOCK_PREF_KEY_ASSERT(kWifiConfigured);
BTCLOCK_PREF_KEY_ASSERT(kWpTimeout);
BTCLOCK_PREF_KEY_ASSERT(kSettingsNs);

#undef BTCLOCK_PREF_KEY_ASSERT

}  // namespace prefs
}  // namespace btclock
