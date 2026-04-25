#include "io/led_prefs.hpp"

#include "settings/pref_keys.hpp"

namespace btclock {
namespace {

// Legacy key strings that lived in the abandoned `led` NVS namespace.
// Kept here (rather than in led_controller.cpp) so the migration path
// is testable from the host suite. Strings match the earlier port
// exactly; do not rename.
constexpr const char* kLegacyKeyBrightness = "brightness";
constexpr const char* kLegacyKeyBlockFlashCol = "blockFlashCol";
constexpr const char* kLegacyKeyDisable = "disable";
constexpr const char* kLegacyKeyFlashUpdate = "flashUpdate";

// Schema-aligned defaults — must match settings/schema.hpp entries for
// kLedBrightness / kBlockFlashColor / kDisableLeds / kLedFlashOnUpd so a
// fresh install lands on the same values the GET /api/settings handler
// reports.
constexpr uint8_t kDefaultBrightness = 128;
constexpr uint32_t kDefaultBlockFlashColor = 0xE04300u;
constexpr bool kDefaultDisabled = false;
constexpr bool kDefaultFlashOnUpdate = false;

}  // namespace

LedPrefsSnapshot ResolveLedPrefs(const settings::PrefsReader& settings_reader,
                                 settings::PrefsWriter& settings_writer,
                                 const settings::PrefsReader& legacy_reader) {
  LedPrefsSnapshot s;

  // Brightness ---------------------------------------------------------
  {
    const uint32_t set_v =
        settings_reader.GetU32(prefs::kLedBrightness, kLedPrefAbsent);
    if (set_v != kLedPrefAbsent) {
      s.brightness = static_cast<uint8_t>(set_v & 0xFFu);
    } else {
      const uint32_t leg =
          legacy_reader.GetU32(kLegacyKeyBrightness, kLedPrefAbsent);
      if (leg != kLedPrefAbsent) {
        s.brightness = static_cast<uint8_t>(leg & 0xFFu);
        settings_writer.SetU32(prefs::kLedBrightness, leg & 0xFFu);
        s.migrated_from_legacy = true;
      } else {
        s.brightness = kDefaultBrightness;
      }
    }
  }

  // Block flash colour -------------------------------------------------
  {
    const uint32_t set_v =
        settings_reader.GetU32(prefs::kBlockFlashColor, kLedPrefAbsent);
    if (set_v != kLedPrefAbsent) {
      s.block_flash_color = set_v & 0x00FFFFFFu;
    } else {
      const uint32_t leg =
          legacy_reader.GetU32(kLegacyKeyBlockFlashCol, kLedPrefAbsent);
      if (leg != kLedPrefAbsent) {
        s.block_flash_color = leg & 0x00FFFFFFu;
        settings_writer.SetU32(prefs::kBlockFlashColor, leg & 0x00FFFFFFu);
        s.migrated_from_legacy = true;
      } else {
        s.block_flash_color = kDefaultBlockFlashColor;
      }
    }
  }

  // disableLeds (bool stored as 0/1 u32) ------------------------------
  {
    const uint32_t set_v =
        settings_reader.GetU32(prefs::kDisableLeds, kLedPrefAbsent);
    if (set_v != kLedPrefAbsent) {
      s.disabled = set_v != 0;
    } else {
      const uint32_t leg =
          legacy_reader.GetU32(kLegacyKeyDisable, kLedPrefAbsent);
      if (leg != kLedPrefAbsent) {
        s.disabled = leg != 0;
        settings_writer.SetBool(prefs::kDisableLeds, s.disabled);
        s.migrated_from_legacy = true;
      } else {
        s.disabled = kDefaultDisabled;
      }
    }
  }

  // ledFlashOnUpd (bool stored as 0/1 u32) ----------------------------
  {
    const uint32_t set_v =
        settings_reader.GetU32(prefs::kLedFlashOnUpd, kLedPrefAbsent);
    if (set_v != kLedPrefAbsent) {
      s.flash_on_update = set_v != 0;
    } else {
      const uint32_t leg =
          legacy_reader.GetU32(kLegacyKeyFlashUpdate, kLedPrefAbsent);
      if (leg != kLedPrefAbsent) {
        s.flash_on_update = leg != 0;
        settings_writer.SetBool(prefs::kLedFlashOnUpd, s.flash_on_update);
        s.migrated_from_legacy = true;
      } else {
        s.flash_on_update = kDefaultFlashOnUpdate;
      }
    }
  }

  return s;
}

}  // namespace btclock
