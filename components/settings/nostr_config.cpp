// See nostr_config.hpp for context. Pure-logic so the host tests can
// drive it through FakePrefs without an IDF link.

#include "settings/nostr_config.hpp"

#include <cstdint>
#include <sstream>

#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace settings {

namespace {
// dataSource == 2 selects the Nostr source per defaults.hpp::DataSourceType.
constexpr uint8_t kDataSourceNostr = 2;

// Split a CSV list, dropping empties. Used for both pubkeys and relay
// URLs; the caller applies its own cap (kMaxZapPubkeys / kMaxNostrRelays).
std::vector<std::string> SplitCsv(const std::string& csv) {
  std::vector<std::string> out;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

// Resolve the canonical relay list from NVS. Plural slot wins; legacy
// singular is the fallback so installs that pre-date the multi-relay
// rollout keep working without an explicit migration step. Defensive
// cap mirrors the PATCH validator so a hand-edited NVS can't drag the
// boot path past the largest-free-block ceiling.
std::vector<std::string> ReadRelayUrls(const PrefsReader& prefs) {
  std::vector<std::string> out;
  const std::string csv = ReadString(prefs, prefs::kNostrRelays);
  if (!csv.empty()) {
    out = SplitCsv(csv);
  } else {
    const std::string legacy = ReadString(prefs, prefs::kNostrRelay);
    if (!legacy.empty()) out.push_back(legacy);
  }
  if (out.size() > kMaxNostrRelays) out.resize(kMaxNostrRelays);
  return out;
}
}  // namespace

NostrSourceConfig ReadNostrSourceConfig(const PrefsReader& prefs) {
  NostrSourceConfig out;
  out.enabled = (ReadU8(prefs, prefs::kDataSource) == kDataSourceNostr);
  out.relay_urls = ReadRelayUrls(prefs);
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
  out.relay_urls = ReadRelayUrls(prefs);

  // Plural slot is canonical; legacy singular is the fallback so an
  // install that pre-dates the multi-pubkey rollout keeps listening on
  // its existing pubkey without a rewrite step. We don't auto-migrate
  // (write the legacy value into the plural slot) on read — the next
  // PATCH naturally writes the plural slot, and reads stay coherent
  // either way through this fallback.
  const std::string csv = ReadString(prefs, prefs::kNostrZapPubkeys);
  if (!csv.empty()) {
    out.zap_pubkeys = SplitCsv(csv);
  } else {
    const std::string legacy = ReadString(prefs, prefs::kNostrZapPubkey);
    if (!legacy.empty()) out.zap_pubkeys.push_back(legacy);
  }
  // Defensive cap — the schema validator on PATCH already rejects > N,
  // but a corrupted NVS slot or a hand-edited CSV could carry more.
  if (out.zap_pubkeys.size() > kMaxZapPubkeys) {
    out.zap_pubkeys.resize(kMaxZapPubkeys);
  }

  out.zap_screen_auto_restore = ReadBool(prefs, prefs::kScrnRestoreZap);
  out.led_flash_on_zap = ReadBool(prefs, prefs::kLedFlashOnZap);
  out.frontlight_flash_on_zap = ReadBool(prefs, prefs::kFlFlashOnZap);
  return out;
}

}  // namespace settings
}  // namespace btclock
