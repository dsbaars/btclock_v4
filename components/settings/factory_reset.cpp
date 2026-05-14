#include "settings/factory_reset.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace settings {
namespace {
constexpr const char* kTag = "factory-reset";
}  // namespace

void PerformWifiReset() {
  ESP_LOGW(kTag, "wiping STA credentials and rebooting");
  {
    // Scope the Prefs handles so nvs_close runs before nvs_commit's
    // partition flush is interrupted by esp_restart(). Both Commit()s
    // are issued while the handles are still open; the destructors
    // close them at scope-exit, before the restart below.
    Prefs net("net");
    // Remove() returns ESP_ERR_NVS_NOT_FOUND if the key was never set;
    // both call sites already treat that as a no-op. `app` (SoftAP
    // password) is intentionally preserved — see header rationale.
    net.Remove("ssid");
    net.Remove("pw");
    net.Commit();

    Prefs settings_ns(prefs::kSettingsNs);
    // wifiConfigured drives the wpTimeout reboot watchdog in
    // init_network.cpp; clearing it puts the device back into "first
    // boot" mode where a portal session won't time-out-reboot before
    // the user has a chance to enter new creds.
    settings_ns.Remove(prefs::kWifiConfigured);
    settings_ns.Commit();
  }
  // Same 2s splash dwell as PerformFactoryReset for parity — the EPD
  // chain needs the full-refresh frame to flush before the CPU drops.
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void PerformFactoryReset() {
  ESP_LOGW(kTag, "erasing NVS partition and rebooting");
  // nvs_flash_erase() wipes every namespace at once — the caller wants
  // a full reset, not a selective one. Deinit first so no handle holds
  // the partition open; ignore errors from deinit because we're about
  // to erase anyway. After erase we DO NOT re-init: we reboot and let
  // prefs::InitOnce() bring up a fresh partition on the next boot.
  nvs_flash_deinit();
  const esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    // Fall through to restart anyway — leaving the user in an
    // indeterminate state is worse than a best-effort retry-on-boot.
  }
  // Two-second hold so the caller's "Resetting…" EPD frame has time to
  // flush to the panels before the CPU drops. Tuned against the ~1.1 s
  // worst-case full-refresh on the V8's 8-panel chain.
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
  // esp_restart() is [[noreturn]] in practice, but the compiler doesn't
  // know that without the attribute on the prototype. Loop forever so
  // the [[noreturn]] contract on our own signature holds even if the
  // reset somehow fails.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace settings
}  // namespace btclock
