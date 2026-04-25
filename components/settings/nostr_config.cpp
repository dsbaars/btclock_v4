// See nostr_config.hpp for context. Pure-logic so the host tests can
// drive it through FakePrefs without an IDF link.

#include "settings/nostr_config.hpp"

#include <cstdint>

#include "settings/pref_keys.hpp"

namespace btclock {
namespace settings {

namespace {
// Defaults mirror schema.hpp::kFields. Keep in sync — a drift here
// silently makes a fresh-install field default different from what
// /api/settings GET reports, which confuses the WebUI.
constexpr const char* kDefaultNostrRelay = "wss://relay.primal.net";
constexpr const char* kDefaultNostrPubKey =
    "642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288";
constexpr const char* kDefaultZapPubkey =
    "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422";
// dataSource == 2 selects the Nostr source per defaults.hpp::DataSourceType.
constexpr uint8_t kDataSourceNostr = 2;
}  // namespace

NostrSourceConfig ReadNostrSourceConfig(const PrefsReader& prefs) {
  NostrSourceConfig out;
  const uint8_t ds = prefs.GetU8(prefs::kDataSource, 0);
  out.enabled = (ds == kDataSourceNostr);
  out.relay_url = prefs.GetString(prefs::kNostrRelay, kDefaultNostrRelay);
  out.author_pubkey_hex =
      prefs.GetString(prefs::kNostrPubKey, kDefaultNostrPubKey);
  return out;
}

ZapListenerConfig ReadZapListenerConfig(const PrefsReader& prefs) {
  ZapListenerConfig out;
  // nostrZapNotify is the master enable for the zap-screen overlay AND
  // (here) for wiring the listener at all — keeping a single toggle
  // matches the WebUI's mental model.
  out.zap_screen_notify =
      prefs.GetBool(prefs::kNostrZapNotify, true);
  out.enabled = out.zap_screen_notify;
  out.relay_url =
      prefs.GetString(prefs::kNostrRelay, kDefaultNostrRelay);
  out.zap_pubkey =
      prefs.GetString(prefs::kNostrZapPubkey, kDefaultZapPubkey);
  out.zap_screen_auto_restore =
      prefs.GetBool(prefs::kScrnRestoreZap, true);
  out.led_flash_on_zap =
      prefs.GetBool(prefs::kLedFlashOnZap, true);
  out.frontlight_flash_on_zap =
      prefs.GetBool(prefs::kFlFlashOnZap, false);
  return out;
}

}  // namespace settings
}  // namespace btclock
