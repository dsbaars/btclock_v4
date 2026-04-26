#include "app/boot/init_storage.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "epd_ssd1680.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "littlefs.hpp"
#include "prefs.hpp"
#include "sdkconfig.h"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "timezone/timezone.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void InitStorage(AppCtx& /*ctx*/) {
  // --- WiFi + NVS + optional provisioning portal ---
  ESP_ERROR_CHECK(Prefs::InitOnce());

  // enableDebugLog: bump the global log level the moment NVS is up so
  // every subsystem started after this point (network, screen manager,
  // data sources) emits at DEBUG. The earlier banner / hardware lines
  // already ran at the default INFO and stay that way for this boot.
  // Reboot is required to re-tighten — matches the SETTINGS.md row.
  {
    Prefs settings(prefs::kSettingsNs);
    if (btclock::settings::ReadBool(settings, prefs::kEnableDebugLog)) {
      esp_log_level_set("*", ESP_LOG_DEBUG);
    }
  }

  // Install the EPD polarity flag from NVS before the first data-driven
  // render. Boot splash above already painted non-inverted; the schema
  // default agrees so a fresh device matches the splash. Flipping the
  // default is a one-line edit in schema.hpp.
  {
    Prefs p(prefs::kSettingsNs);
    EpdSetGlobalInverted(btclock::settings::ReadBool(p, prefs::kInvertedColor));
  }

  // Set the process-wide TZ from NVS (namespace "time", key "tz")
  // before anything that calls localtime_r. The clock screen, log
  // timestamps, and any future scheduling code all rely on it being
  // set; if the stored value is missing or unknown we fall back to
  // UTC and log the reason. setenv/tzset don't need the network.
  //
  // TODO(beads): wire /api/settings write-back so the WebUI can
  // change the zone at runtime — that lives in the jwz epic, not
  // here. This call only restores whatever's already in NVS.
  timezone::InitFromNvs();

  // LittleFS is used for the future static-WebUI bundle and OTA-webui
  // uploads. We format-on-failure so a blank partition (fresh flash,
  // first boot after partition-table change) self-heals. A mount error
  // after that fallback is logged but non-fatal — the firmware should
  // continue to boot without a filesystem rather than brick.
  const esp_err_t lfs_err = MountLittleFs(kLittleFsDefaultBasePath);
  if (lfs_err != ESP_OK) {
    ESP_LOGE(kTag, "LittleFS mount failed (%s); continuing without FS",
             esp_err_to_name(lfs_err));
  }
#ifdef CONFIG_BTCLOCK_LITTLEFS_SELFTEST
  if (lfs_err == ESP_OK) {
    RunLittleFsSelfTest(kLittleFsDefaultBasePath);
  }
#endif
}

}  // namespace btclock
