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

inline constexpr const char* kActCurrencies = "actCurrencies";
inline constexpr const char* kBgColor = "bgColor";
inline constexpr const char* kBitaxeEnabled = "bitaxeEnabled";
inline constexpr const char* kBitaxeHostname = "bitaxeHostname";
inline constexpr const char* kBitaxePollSec = "bitaxePollSec";
inline constexpr const char* kBlockFeeDec = "blockFeeDec";
inline constexpr const char* kBlockFlashColor = "blockFlashColor";
inline constexpr const char* kBlockHeight = "blockHeight";
inline constexpr const char* kCeDisableSSL = "ceDisableSSL";
inline constexpr const char* kCeEndpoint = "ceEndpoint";
inline constexpr const char* kCurrentScreen = "currentScreen";
inline constexpr const char* kDataSource = "dataSource";
// Pixel height for the "big" digit glyphs on data screens (block height,
// BTC price, fee rate, mining-pool earnings, etc.). Default 180 matches
// the historical kDigitPx baseline; the schema bounds (80..220) leave
// horizontal headroom for Antonio's widest digit ink (~0.55 * px) within
// the 122 px short axis. Runtime — on_settings_patched marks the screen
// dirty so the next paint picks up the new value.
inline constexpr const char* kDigitFontPx = "digitFontPx";
inline constexpr const char* kDisableLeds = "disableLeds";
inline constexpr const char* kDndEnabled = "dndEnabled";
inline constexpr const char* kDndEndHour = "dndEndHour";
inline constexpr const char* kDndEndMin = "dndEndMin";
inline constexpr const char* kDndStartHour = "dndStartHour";
inline constexpr const char* kDndStartMin = "dndStartMin";
inline constexpr const char* kDndTimeEnabled = "dndTimeEnabled";
inline constexpr const char* kEnableDebugLog = "enableDebugLog";
inline constexpr const char* kFgColor = "fgColor";
inline constexpr const char* kFlAlwaysOn = "flAlwaysOn";
inline constexpr const char* kFlDisable = "flDisable";
inline constexpr const char* kFlEffectDelay = "flEffectDelay";
inline constexpr const char* kFlFlashOnUpd = "flFlashOnUpd";
inline constexpr const char* kFlFlashOnZap = "flFlashOnZap";
inline constexpr const char* kFlMaxBrightness = "flMaxBrightness";
inline constexpr const char* kFlOffWhenDark = "flOffWhenDark";
inline constexpr const char* kFontName = "fontName";
// Foundry USA Pool: per-account API key (suppressed in GET like
// httpAuthPass) + plain subaccount path segment. NVS 15-char cap forces
// the short forms — `foundryApiKey` is exactly 13 chars; `foundrySubacct`
// is 14. Stable on-flash names; do not rename.
inline constexpr const char* kFullRefreshMin = "fullRefreshMin";
inline constexpr const char* kGitReleaseUrl = "gitReleaseUrl";
inline constexpr const char* kGmtOffset = "gmtOffset";
// Clock screen: drop the leading zero on single-digit hours
// ("07:00" → "7:00"). Minute always stays zero-padded. NVS 15-char
// cap forces the short form — the old "hideTimeLeadingZero" name
// would need 19 bytes. Key is stable once shipped; do not rename.
inline constexpr const char* kHideLeadZero = "hideLeadZero";
inline constexpr const char* kHostnamePrefix = "hostnamePrefix";
inline constexpr const char* kHttpAuthEnabled = "httpAuthEnabled";
inline constexpr const char* kHttpAuthPass = "httpAuthPass";
inline constexpr const char* kHttpAuthUser = "httpAuthUser";
inline constexpr const char* kInverseButtons = "inverseButtons";
inline constexpr const char* kInvertedColor = "invertedColor";
inline constexpr const char* kLedBrightness = "ledBrightness";
inline constexpr const char* kLedFlashOnUpd = "ledFlashOnUpd";
inline constexpr const char* kLedFlashOnZap = "ledFlashOnZap";
inline constexpr const char* kLedTestOnPower = "ledTestOnPower";
inline constexpr const char* kLocalPoolHost = "localPoolHost";
inline constexpr const char* kLuxLightToggle = "luxLightToggle";
inline constexpr const char* kMcapBigChar = "mcapBigChar";
inline constexpr const char* kMdnsEnabled = "mdnsEnabled";
inline constexpr const char* kMempoolInstance = "mempoolInstance";
inline constexpr const char* kMempoolSecure = "mempoolSecure";
inline constexpr const char* kMinSecPriceUpd = "minSecPriceUpd";
inline constexpr const char* kMiningPoolName = "miningPoolName";
inline constexpr const char* kMiningPoolStats = "miningPoolStats";
inline constexpr const char* kMiningPoolUser = "miningPoolUser";
inline constexpr const char* kPoolWorker = "poolWorker";
inline constexpr const char* kMowMode = "mowMode";
inline constexpr const char* kNostrPubKey = "nostrPubKey";
inline constexpr const char* kNostrRelay = "nostrRelay";
inline constexpr const char* kNostrZapNotify = "nostrZapNotify";
inline constexpr const char* kNostrZapPubkey = "nostrZapPubkey";
inline constexpr const char* kOtaEnabled = "otaEnabled";
inline constexpr const char* kOtaPass = "otaPass";
inline constexpr const char* kPoolGlobalStats = "poolGlobalStats";
inline constexpr const char* kPoolLogosUrl = "poolLogosUrl";
inline constexpr const char* kPoolPollSec = "poolPollSec";
// Outbound proxy — applied at every esp_http_client / esp_websocket_client
// init point. proxyType: 0=none, 1=HTTP CONNECT, 2=SOCKS4, 3=SOCKS4a, 4=SOCKS5.
// Auth (proxyUser/Pass) is honoured for HTTP CONNECT and SOCKS5 only.
// proxyBypass: comma-separated globs evaluated against destination hostname,
// e.g. "*.local,192.168.*"; matching destinations connect direct.
inline constexpr const char* kProxyEnabled = "proxyEnabled";
inline constexpr const char* kProxyType = "proxyType";
inline constexpr const char* kProxyHost = "proxyHost";
inline constexpr const char* kProxyPort = "proxyPort";
inline constexpr const char* kProxyUser = "proxyUser";
inline constexpr const char* kProxyPass = "proxyPass";
inline constexpr const char* kProxyBypass = "proxyBypass";
inline constexpr const char* kRefrScrnChange = "refrScrnChange";
// Sats-symbol variant (0..15) — index into the 16 glyphs at U+E000..U+E00F
// of the SatoshiSymbol font. Default 7 matches the production glyph that
// shipped before the variant pref existed. PATCH-able via /api/settings;
// runtime hook live-updates the renderer without a reboot.
inline constexpr const char* kSatsVariant = "satsVariant";
inline constexpr const char* kScreenOrder = "screenOrder";
inline constexpr const char* kScrnRestoreZap = "scrnRestoreZap";
inline constexpr const char* kStealFocus = "stealFocus";
inline constexpr const char* kSuffixPrice = "suffixPrice";
// Decimal-point packing — when true, fold the '.' into the digit cell
// before it instead of using a dedicated panel. Applies anywhere the
// price layout includes a decimal point: the K/M-suffix path on the BTC
// price screen, the same path on the market-cap big-chars screen, and
// the sub-1 sat-per-currency "0.dddd" layout on the SATS/<CCY> screen.
//
// Renamed 2026-05-05 from `suffixShareDot` (the old name implied K/M
// suffixes only — the same dot-folding applies to every decimal layout
// the screens produce). The 15-char NVS limit forced one of these
// shorter forms; `decimalShareDot` describes both regimes precisely.
// Legacy `suffixShareDot` value is auto-migrated at boot — see
// settings_migration.cpp.
inline constexpr const char* kDecimalShareDot = "decimalShareDot";
// Pre-rename name. Keep readable so a one-shot migration can copy the
// value forward; never written by new code.
inline constexpr const char* kSuffixShareDot_Legacy = "suffixShareDot";
inline constexpr const char* kSupplyPercent = "supplyPercent";
inline constexpr const char* kTimerActive = "timerActive";
inline constexpr const char* kTimerSeconds = "timerSeconds";
inline constexpr const char* kTxPower = "txPower";
inline constexpr const char* kTzString = "tzString";
inline constexpr const char* kUseBlkCountdown = "useBlkCountdown";
inline constexpr const char* kUseMscwTime = "useMscwTime";
inline constexpr const char* kUseSatsSymbol = "useSatsSymbol";
inline constexpr const char* kVerticalDesc = "verticalDesc";
// ViaBTC: per-account API key (suppressed in GET like httpAuthPass).
// 13 chars — fits the NVS 15-char cap. Stable on-flash name; do not rename.
inline constexpr const char* kWifiConfigured = "wifiConfigured";
// Minutes of continuous STA disconnect before a soft reboot. Ported
// from the Arduino main.cpp::checkWiFiConnection() 10-min brute-force
// recovery path; 0 disables. Key is truncated to the NVS 15-char cap.
inline constexpr const char* kWifiRebootMin = "wifiRebootMin";
inline constexpr const char* kWpTimeout = "wpTimeout";

// NVS namespace that holds every key listed above. One namespace keeps
// settings migration atomic — a reset blows the whole thing away, and
// a backup can be taken with nvs_entry_find("nvs", kSettingsNs, ...).
inline constexpr const char* kSettingsNs = "settings";

// Runtime-state namespace — slot/rotation cursor that lives across
// reboots so the device resumes where it was rather than always
// booting on slot 0 (block height). Kept separate from `kSettingsNs`
// because this isn't user-PATCH-able and we don't want it surfacing
// on /api/settings or in factory-reset round-trips on the same axis.
inline constexpr const char* kRuntimeStateNs = "rt";
// Last rendered slot index. Written from the main task via
// PublishStatus when slot_ changes; read at boot in
// init_screen_manager so a reboot resumes the user's last cursor.
inline constexpr const char* kLastSlot = "lastSlot";
// Last seen block height. Persisted from the main task on every
// validated block-height update so a reboot starts with the height
// the device was tracking instead of the empty `last_seen_height_=0`
// that suppresses the very first WS-frame's new-block reaction.
// Mirrors v3 commit 989e645 ("fix: Fix block number caching").
inline constexpr const char* kLastBlockHeight = "lastBlockHt";

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
BTCLOCK_PREF_KEY_ASSERT(kBitaxePollSec);
BTCLOCK_PREF_KEY_ASSERT(kBlockFeeDec);
BTCLOCK_PREF_KEY_ASSERT(kBlockFlashColor);
BTCLOCK_PREF_KEY_ASSERT(kBlockHeight);
BTCLOCK_PREF_KEY_ASSERT(kCeDisableSSL);
BTCLOCK_PREF_KEY_ASSERT(kCeEndpoint);
BTCLOCK_PREF_KEY_ASSERT(kCurrentScreen);
BTCLOCK_PREF_KEY_ASSERT(kDataSource);
BTCLOCK_PREF_KEY_ASSERT(kDigitFontPx);
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
BTCLOCK_PREF_KEY_ASSERT(kHideLeadZero);
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
BTCLOCK_PREF_KEY_ASSERT(kPoolWorker);
BTCLOCK_PREF_KEY_ASSERT(kMowMode);
BTCLOCK_PREF_KEY_ASSERT(kNostrPubKey);
BTCLOCK_PREF_KEY_ASSERT(kNostrRelay);
BTCLOCK_PREF_KEY_ASSERT(kNostrZapNotify);
BTCLOCK_PREF_KEY_ASSERT(kNostrZapPubkey);
BTCLOCK_PREF_KEY_ASSERT(kOtaEnabled);
BTCLOCK_PREF_KEY_ASSERT(kOtaPass);
BTCLOCK_PREF_KEY_ASSERT(kPoolGlobalStats);
BTCLOCK_PREF_KEY_ASSERT(kPoolLogosUrl);
BTCLOCK_PREF_KEY_ASSERT(kPoolPollSec);
BTCLOCK_PREF_KEY_ASSERT(kRefrScrnChange);
BTCLOCK_PREF_KEY_ASSERT(kSatsVariant);
BTCLOCK_PREF_KEY_ASSERT(kScreenOrder);
BTCLOCK_PREF_KEY_ASSERT(kScrnRestoreZap);
BTCLOCK_PREF_KEY_ASSERT(kStealFocus);
BTCLOCK_PREF_KEY_ASSERT(kSuffixPrice);
BTCLOCK_PREF_KEY_ASSERT(kDecimalShareDot);
BTCLOCK_PREF_KEY_ASSERT(kSuffixShareDot_Legacy);
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
BTCLOCK_PREF_KEY_ASSERT(kWifiRebootMin);
BTCLOCK_PREF_KEY_ASSERT(kWpTimeout);
BTCLOCK_PREF_KEY_ASSERT(kSettingsNs);
BTCLOCK_PREF_KEY_ASSERT(kRuntimeStateNs);
BTCLOCK_PREF_KEY_ASSERT(kLastSlot);
BTCLOCK_PREF_KEY_ASSERT(kLastBlockHeight);

#undef BTCLOCK_PREF_KEY_ASSERT

}  // namespace prefs
}  // namespace btclock
