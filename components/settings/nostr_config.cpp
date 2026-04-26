// See nostr_config.hpp for context. Pure-logic so the host tests can
// drive it through FakePrefs without an IDF link.

#include "settings/nostr_config.hpp"

#include <cstdint>

#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace settings {

namespace {
// dataSource == 2 selects the Nostr source per defaults.hpp::DataSourceType.
constexpr uint8_t kDataSourceNostr = 2;
}  // namespace

NostrSourceConfig ReadNostrSourceConfig(const PrefsReader& prefs) {
  NostrSourceConfig out;
  out.enabled = (ReadU8(prefs, prefs::kDataSource) == kDataSourceNostr);
  out.relay_url = ReadString(prefs, prefs::kNostrRelay);
  out.author_pubkey_hex = ReadString(prefs, prefs::kNostrPubKey);
  return out;
}

ZapListenerConfig ReadZapListenerConfig(const PrefsReader& prefs) {
  ZapListenerConfig out;
  // nostrZapNotify is the master enable for the zap-screen overlay AND
  // (here) for wiring the listener at all — keeping a single toggle
  // matches the WebUI's mental model.
  out.zap_screen_notify = ReadBool(prefs, prefs::kNostrZapNotify);
  out.enabled = out.zap_screen_notify;
  out.relay_url = ReadString(prefs, prefs::kNostrRelay);
  out.zap_pubkey = ReadString(prefs, prefs::kNostrZapPubkey);
  out.zap_screen_auto_restore = ReadBool(prefs, prefs::kScrnRestoreZap);
  out.led_flash_on_zap = ReadBool(prefs, prefs::kLedFlashOnZap);
  out.frontlight_flash_on_zap = ReadBool(prefs, prefs::kFlFlashOnZap);
  return out;
}

}  // namespace settings
}  // namespace btclock
