#include "timezone/timezone.hpp"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock::timezone {
namespace {
constexpr const char* kTag = "tz";
}  // namespace

esp_err_t SetTimezoneByName(const char* iana_name) {
  if (iana_name == nullptr || iana_name[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  const std::string_view posix = PosixForIana(iana_name);
  if (posix.empty()) {
    ESP_LOGW(kTag, "unknown zone '%s'", iana_name);
    return ESP_ERR_NOT_FOUND;
  }

  // std::string_view isn't null-terminated in general, but kTzTable's
  // entries come from string literals which *are*. Still, take a copy
  // to pass to setenv since the contract requires a C string.
  const std::string posix_copy(posix);
  setenv("TZ", posix_copy.c_str(), /*overwrite=*/1);
  tzset();
  ESP_LOGI(kTag, "set TZ: %s -> %s", iana_name, posix_copy.c_str());

  // Persist for next boot. Deliberately non-fatal if NVS is wedged —
  // the user's current session still has the correct zone. We only
  // write when the value changed to avoid pointless wear.
  esp_err_t persist_err = ESP_OK;
  {
    btclock::Prefs prefs(kNvsNamespace);
    const std::string current = prefs.GetString(kNvsKey, "");
    if (current != iana_name) {
      persist_err = prefs.SetString(kNvsKey, iana_name);
      if (persist_err == ESP_OK) persist_err = prefs.Commit();
    }
  }
  return persist_err;
}

std::string GetTimezoneName() {
  // The settings subsystem writes the PATCHed `tzString` into the
  // "settings" namespace alongside the rest of the WebUI-exposed prefs.
  // settings::ReadString returns the schema's default ("Europe/Amsterdam"
  // per schema.hpp::kFields) when the slot is missing or empty, so a
  // fresh device matches what GET /api/settings reports.
  {
    btclock::Prefs settings_prefs(kSettingsNamespace);
    const std::string from_settings = btclock::settings::ReadString(
        settings_prefs, btclock::prefs::kTzString);
    if (!from_settings.empty()) return from_settings;
  }
  // Legacy fallback — only reachable if the schema default is also
  // empty (it isn't today). Kept so an unforeseen schema regression
  // doesn't strand devices that still have the old "time"/"tz" slot.
  btclock::Prefs prefs(kNvsNamespace);
  std::string value = prefs.GetString(kNvsKey, kDefaultZone);
  if (value.empty()) value = kDefaultZone;
  return value;
}

void InitFromNvs() {
  const std::string zone = GetTimezoneName();
  const esp_err_t err = SetTimezoneByName(zone.c_str());
  if (err == ESP_OK) return;
  if (err == ESP_ERR_NOT_FOUND) {
    ESP_LOGW(kTag, "stored zone '%s' unknown; falling back to %s", zone.c_str(),
             kDefaultZone);
  } else {
    ESP_LOGW(kTag, "SetTimezoneByName('%s') -> %s; falling back to %s",
             zone.c_str(), esp_err_to_name(err), kDefaultZone);
  }
  // Fallback: UTC is always present in the table.
  (void)SetTimezoneByName(kDefaultZone);
}

}  // namespace btclock::timezone
