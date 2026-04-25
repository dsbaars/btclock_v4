// Pure-logic loader for the four user-tunable LED prefs:
//   - ledBrightness  (uint8, 0..255, default 128)
//   - blockFlashColor (uint32 0xRRGGBB, default 0xE04300)
//   - disableLeds    (bool, default false)
//   - ledFlashOnUpd  (bool, default false)
//
// Production reads them out of the `settings` NVS namespace using the
// same key strings the WebUI PATCHes (see settings/pref_keys.hpp). The
// older firmware port mirrored a separate `led` namespace with the
// same intent but different key strings; this helper migrates legacy
// values forward on first read so an existing install keeps the
// brightness / disable / flash-on-update toggles it had pre-migration.
//
// Pure-logic so test_host can drive it against the in-memory FakePrefs
// without linking ESP-IDF. The IDF caller (led_controller.cpp) wraps a
// real NvsPrefs around `settings` and a small legacy adapter for `led`.

#pragma once

#include <cstdint>

#include "settings/api.hpp"

namespace btclock {

// Resolved snapshot of the four LED prefs after read + migration.
struct LedPrefsSnapshot {
  uint8_t brightness = 128;
  uint32_t block_flash_color = 0xE04300;
  bool disabled = false;
  bool flash_on_update = false;
  // True when at least one legacy `led/*` key was promoted into the
  // `settings/*` namespace. The IDF caller commits + (optionally) erases
  // the legacy keys when this is set.
  bool migrated_from_legacy = false;
};

// Sentinel u32 used to detect "key absent" via PrefsReader::GetU32.
// 0xFFFFFFFF is outside every valid LED-pref range (brightness clamps to
// 0..255, blockFlashColor masks to 0x00FFFFFF, bools store 0/1) so it
// uniquely flags an unwritten slot.
inline constexpr uint32_t kLedPrefAbsent = 0xFFFFFFFFu;

// Read the four LED prefs from `settings_reader`. Any key missing from
// settings is filled from `legacy_reader` (the old `led` namespace) if
// the legacy slot has a value; otherwise the schema defaults apply. When
// a legacy value wins, it is also written into `settings_writer` so the
// next boot reads it directly from the new namespace and the WebUI
// PATCH path keeps a single source of truth.
LedPrefsSnapshot ResolveLedPrefs(const settings::PrefsReader& settings_reader,
                                 settings::PrefsWriter& settings_writer,
                                 const settings::PrefsReader& legacy_reader);

}  // namespace btclock
