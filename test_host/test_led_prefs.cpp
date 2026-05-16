// Host tests for the LED prefs loader (components/led_strip_logic/led_prefs.cpp).
//
// Covers two responsibilities of ResolveLedPrefs:
//   1. PATCH read-back — when the WebUI writes a key into the
//      `settings` namespace, the LED controller should pick up the new
//      value on its next boot. The earlier port read its own
//      `led` namespace and ignored the settings PATCHes entirely
//      (bd btclock_v4-xfm).
//   2. Migration — installs that pre-date the unified-namespace switch
//      have legacy values under the old `led` namespace; the loader
//      promotes them into `settings/*` on first read so the user
//      doesn't lose their saved brightness / disable / flash-on-update
//      state.
//
// Pure-logic — uses the same FakePrefs shape as test_settings_api.cpp
// against the PrefsReader/PrefsWriter abstraction.

#include <cstdint>
#include <map>
#include <string>

#include "doctest.h"
#include "led_strip_logic/led_prefs.hpp"
#include "settings/api.hpp"
#include "settings/pref_keys.hpp"

namespace {

class FakePrefs final : public btclock::settings::PrefsReader,
                        public btclock::settings::PrefsWriter {
 public:
  std::string GetString(const char* key,
                        const char* default_value) const override {
    auto it = str_.find(key);
    return it != str_.end() ? it->second : (default_value ? default_value : "");
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
    auto it = u32_.find(key);
    return it != u32_.end() ? (it->second != 0) : default_value;
  }

  void SetString(const char* key, const char* value) override {
    str_[key] = value ? value : "";
  }
  void SetU32(const char* key, uint32_t value) override { u32_[key] = value; }
  void SetI32(const char* key, int32_t value) override { i32_[key] = value; }
  void SetU8(const char* key, uint8_t value) override { u8_[key] = value; }
  // Bool-as-u32 lines up with how the IDF NvsPrefs adapter stores
  // booleans (Prefs::SetBool wraps SetU32 with 0/1) so the absent /
  // present distinction the loader relies on still works.
  void SetBool(const char* key, bool value) override {
    u32_[key] = value ? 1u : 0u;
  }
  void Remove(const char* key) override {
    str_.erase(key);
    u32_.erase(key);
    i32_.erase(key);
    u8_.erase(key);
  }

  std::map<std::string, std::string> str_;
  std::map<std::string, uint32_t> u32_;
  std::map<std::string, int32_t> i32_;
  std::map<std::string, uint8_t> u8_;
};

}  // namespace

// -------------------------------------------------------------------
// PATCH read-back: settings/* keys win over both legacy and defaults.
// -------------------------------------------------------------------

TEST_CASE("ledBrightness PATCH is observed by the LED controller") {
  FakePrefs settings_ns, legacy_ns;
  settings_ns.SetU32(btclock::prefs::kLedBrightness, 42);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.brightness == 42);
  CHECK_FALSE(s.migrated_from_legacy);
}

TEST_CASE("disableLeds PATCH is observed by the LED controller") {
  FakePrefs settings_ns, legacy_ns;
  settings_ns.SetBool(btclock::prefs::kDisableLeds, true);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.disabled);
  CHECK_FALSE(s.migrated_from_legacy);
}

TEST_CASE("ledFlashOnUpd PATCH (false) is observed by the LED controller") {
  FakePrefs settings_ns, legacy_ns;
  // Schema default for ledFlashOnUpd is true (raised in v4 to match
  // flFlashOnUpd) — explicitly write false and confirm the loader honours
  // the PATCHed value rather than falling back to the schema default.
  settings_ns.SetBool(btclock::prefs::kLedFlashOnUpd, false);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK_FALSE(s.flash_on_update);
  CHECK_FALSE(s.migrated_from_legacy);
}

TEST_CASE("ledFlashOnUpd PATCH (true) is observed by the LED controller") {
  FakePrefs settings_ns, legacy_ns;
  settings_ns.SetBool(btclock::prefs::kLedFlashOnUpd, true);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.flash_on_update);
  CHECK_FALSE(s.migrated_from_legacy);
}

TEST_CASE("blockFlashColor PATCH is observed by the LED controller") {
  FakePrefs settings_ns, legacy_ns;
  settings_ns.SetU32(btclock::prefs::kBlockFlashColor, 0x123456u);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.block_flash_color == 0x123456u);
}

// -------------------------------------------------------------------
// Defaults: empty-everywhere → schema defaults.
// -------------------------------------------------------------------

TEST_CASE("Empty NVS yields schema defaults") {
  FakePrefs settings_ns, legacy_ns;

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.brightness == 128);
  CHECK(s.block_flash_color == 0xE04300u);
  CHECK_FALSE(s.disabled);
  // ledFlashOnUpd default is true (matches flFlashOnUpd).
  CHECK(s.flash_on_update);
  CHECK_FALSE(s.migrated_from_legacy);
}

// -------------------------------------------------------------------
// Migration: legacy value present, settings empty → loader promotes it.
// -------------------------------------------------------------------

TEST_CASE("Legacy 'led/brightness' migrates into settings on first boot") {
  FakePrefs settings_ns, legacy_ns;
  legacy_ns.SetU32("brightness", 64);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.brightness == 64);
  CHECK(s.migrated_from_legacy);
  // Legacy value was promoted into settings_ns so the next boot reads
  // it from there directly.
  CHECK(settings_ns.GetU32(btclock::prefs::kLedBrightness, 0) == 64u);
}

TEST_CASE("Legacy 'led/disable' (true) migrates into settings") {
  FakePrefs settings_ns, legacy_ns;
  legacy_ns.SetBool("disable", true);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.disabled);
  CHECK(s.migrated_from_legacy);
  CHECK(settings_ns.GetBool(btclock::prefs::kDisableLeds, false));
}

TEST_CASE("Legacy 'led/flashUpdate' migrates into settings") {
  FakePrefs settings_ns, legacy_ns;
  // Earlier default for flashUpdate was true — that's the case we
  // most care about preserving across the migration.
  legacy_ns.SetBool("flashUpdate", true);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.flash_on_update);
  CHECK(s.migrated_from_legacy);
  CHECK(settings_ns.GetBool(btclock::prefs::kLedFlashOnUpd, false));
}

TEST_CASE("Settings value wins over legacy when both are present") {
  FakePrefs settings_ns, legacy_ns;
  settings_ns.SetU32(btclock::prefs::kLedBrightness, 99);
  legacy_ns.SetU32("brightness", 1);

  const auto s = btclock::ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);

  CHECK(s.brightness == 99);
  CHECK_FALSE(s.migrated_from_legacy);
}
