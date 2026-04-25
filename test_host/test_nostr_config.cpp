// Host tests for ReadNostrSourceConfig + ReadZapListenerConfig
// (components/settings/nostr_config.cpp). The shipping bug those
// readers fix: PATCH /api/settings writes nostr keys into the
// canonical "settings" NVS namespace, but the on-device data-source
// builder + zap-listener used to read from a separate "nostr"
// namespace with shorthand keys, so the WebUI's PATCH was a no-op.
// These tests pin the namespace + key-name contract: a value the
// PATCH layer wrote MUST be the same value the readers see.
//
// Pure-logic — both readers take a PrefsReader so the host test can
// inject an in-memory FakePrefs and the FakePrefs' map keys are the
// schema's NVS key names (kNostrRelay, kNostrZapPubkey, …) verbatim.
//
// bd btclock_v4-aw5 (NostrDataSource) + btclock_v4-q1l (ZapListener).

#include "doctest.h"

#include <cstdint>
#include <map>
#include <string>

#include "settings/api.hpp"
#include "settings/nostr_config.hpp"
#include "settings/pref_keys.hpp"

namespace {

// Same shape FakePrefs as test_settings_api.cpp uses; duplicated here
// so the two test files can fail independently.
class FakePrefs final : public btclock::settings::PrefsReader,
                        public btclock::settings::PrefsWriter {
 public:
  std::string GetString(const char* key,
                        const char* default_value) const override {
    auto it = str_.find(key);
    return it != str_.end() ? it->second
                            : (default_value ? default_value : "");
  }
  uint32_t GetU32(const char* key, uint32_t default_value) const override {
    auto it = u32_.find(key);
    return it != u32_.end() ? it->second : default_value;
  }
  int32_t GetI32(const char* key, int32_t default_value) const override {
    auto it = i32_.find(key);
    return it != i32_.end() ? it->second : default_value;
  }
  uint8_t GetU8(const char* key, uint8_t default_value) const override {
    auto it = u8_.find(key);
    return it != u8_.end() ? it->second : default_value;
  }
  bool GetBool(const char* key, bool default_value) const override {
    auto it = b_.find(key);
    return it != b_.end() ? it->second : default_value;
  }

  void SetString(const char* key, const char* value) override {
    str_[key] = value ? value : "";
  }
  void SetU32(const char* key, uint32_t value) override { u32_[key] = value; }
  void SetI32(const char* key, int32_t value) override { i32_[key] = value; }
  void SetU8(const char* key, uint8_t value) override { u8_[key] = value; }
  void SetBool(const char* key, bool value) override { b_[key] = value; }
  void Remove(const char* key) override {
    str_.erase(key);
    u32_.erase(key);
    i32_.erase(key);
    u8_.erase(key);
    b_.erase(key);
  }

  std::map<std::string, std::string> str_;
  std::map<std::string, uint32_t> u32_;
  std::map<std::string, int32_t> i32_;
  std::map<std::string, uint8_t> u8_;
  std::map<std::string, bool> b_;
};

}  // namespace

TEST_CASE("ReadNostrSourceConfig: defaults match schema (no NVS writes)") {
  FakePrefs prefs;
  const auto cfg = btclock::settings::ReadNostrSourceConfig(prefs);
  // dataSource defaults to 0 (BTCLOCK_SOURCE), so the Nostr source is
  // disabled even though kNostrRelay/kNostrPubKey carry sensible
  // defaults. This is the on-disk shape of a fresh install.
  CHECK_FALSE(cfg.enabled);
  CHECK(cfg.relay_url == "wss://relay.primal.net");
  CHECK(cfg.author_pubkey_hex ==
        "642317135fd4c4205323b9dea8af3270657e62d51dc31a657c0ec8aab31c6288");
}

TEST_CASE(
    "ReadNostrSourceConfig: PATCH-style writes round-trip through schema "
    "key names") {
  // Simulate what /api/settings PATCH does: it writes via the canonical
  // schema key names into the prefs writer. The reader must observe
  // those exact keys — that's the byte-for-byte contract bd
  // btclock_v4-aw5 broke.
  FakePrefs prefs;
  prefs.SetU8(btclock::prefs::kDataSource, 2);  // 2 == NostrDataSourceType
  prefs.SetString(btclock::prefs::kNostrRelay, "wss://relay.example.com");
  const std::string pub(64, 'a');
  prefs.SetString(btclock::prefs::kNostrPubKey, pub.c_str());

  const auto cfg = btclock::settings::ReadNostrSourceConfig(prefs);
  CHECK(cfg.enabled);
  CHECK(cfg.relay_url == "wss://relay.example.com");
  CHECK(cfg.author_pubkey_hex == pub);
}

TEST_CASE(
    "ReadNostrSourceConfig: dataSource != 2 keeps the source disabled") {
  // Even with valid relay + pubkey, only dataSource=2 (Nostr) flips
  // `enabled` on. dataSource=0 is the v2 WS source (default), 1 is
  // unimplemented/fallback, 3 is custom-endpoint.
  FakePrefs prefs;
  prefs.SetU8(btclock::prefs::kDataSource, 0);
  prefs.SetString(btclock::prefs::kNostrRelay, "wss://relay.example.com");
  prefs.SetString(btclock::prefs::kNostrPubKey, std::string(64, 'a').c_str());
  CHECK_FALSE(btclock::settings::ReadNostrSourceConfig(prefs).enabled);

  prefs.SetU8(btclock::prefs::kDataSource, 1);
  CHECK_FALSE(btclock::settings::ReadNostrSourceConfig(prefs).enabled);

  prefs.SetU8(btclock::prefs::kDataSource, 3);
  CHECK_FALSE(btclock::settings::ReadNostrSourceConfig(prefs).enabled);

  prefs.SetU8(btclock::prefs::kDataSource, 2);
  CHECK(btclock::settings::ReadNostrSourceConfig(prefs).enabled);
}

TEST_CASE("ReadZapListenerConfig: defaults match schema (no NVS writes)") {
  FakePrefs prefs;
  const auto cfg = btclock::settings::ReadZapListenerConfig(prefs);
  CHECK(cfg.zap_screen_notify);             // default true
  CHECK(cfg.enabled);                       // gated on zap_screen_notify
  CHECK(cfg.relay_url == "wss://relay.primal.net");
  CHECK(cfg.zap_pubkey ==
        "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422");
  CHECK(cfg.zap_screen_auto_restore);       // default true
  CHECK(cfg.led_flash_on_zap);              // default true
  CHECK_FALSE(cfg.frontlight_flash_on_zap); // default false
}

TEST_CASE(
    "ReadZapListenerConfig: PATCH nostrRelay round-trips through schema "
    "key names") {
  // bd btclock_v4-q1l: this is the main regression. Before the fix the
  // listener opened "nostr"/"zapRelay" while PATCH wrote
  // "settings"/"nostrRelay", so a fresh URL never reached the
  // RelayClient. Pin the canonical key path so a future rename can't
  // silently re-introduce the divergence.
  FakePrefs prefs;
  prefs.SetString(btclock::prefs::kNostrRelay, "wss://relay.example.com");
  const auto cfg = btclock::settings::ReadZapListenerConfig(prefs);
  CHECK(cfg.relay_url == "wss://relay.example.com");
}

TEST_CASE(
    "ReadZapListenerConfig: PATCH nostrZapPubkey round-trips through "
    "schema key names") {
  FakePrefs prefs;
  const std::string pub(64, 'a');
  prefs.SetString(btclock::prefs::kNostrZapPubkey, pub.c_str());
  const auto cfg = btclock::settings::ReadZapListenerConfig(prefs);
  CHECK(cfg.zap_pubkey == pub);
}

TEST_CASE(
    "ReadZapListenerConfig: nostrZapNotify is the master enable") {
  // Toggling nostrZapNotify=false also flips `enabled` so the
  // listener's "should we wire this up at all" check sees a single
  // source of truth.
  FakePrefs prefs;
  prefs.SetBool(btclock::prefs::kNostrZapNotify, false);
  const auto cfg = btclock::settings::ReadZapListenerConfig(prefs);
  CHECK_FALSE(cfg.zap_screen_notify);
  CHECK_FALSE(cfg.enabled);
}

TEST_CASE(
    "ReadZapListenerConfig: flash gates round-trip through schema key "
    "names") {
  // The pre-fix code read "nostr"/"flashOnZap" + "frontlight"/"flFlashOnZap"
  // — both shorthand and in different namespaces. Schema canonicalises
  // them in the "settings" namespace under kLedFlashOnZap +
  // kFlFlashOnZap, so a PATCH to either key MUST land in the listener
  // atomic on the next refresh.
  FakePrefs prefs;
  prefs.SetBool(btclock::prefs::kLedFlashOnZap, false);
  prefs.SetBool(btclock::prefs::kFlFlashOnZap, true);
  prefs.SetBool(btclock::prefs::kScrnRestoreZap, false);
  const auto cfg = btclock::settings::ReadZapListenerConfig(prefs);
  CHECK_FALSE(cfg.led_flash_on_zap);
  CHECK(cfg.frontlight_flash_on_zap);
  CHECK_FALSE(cfg.zap_screen_auto_restore);
}
