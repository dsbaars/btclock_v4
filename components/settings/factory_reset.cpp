#include "settings/factory_reset.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace btclock {
namespace settings {
namespace {
constexpr const char* kTag = "factory-reset";
}  // namespace

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
