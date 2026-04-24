#include "app/wifi_guard.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "prefs.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "wifi-guard";
}  // namespace

void WaitForConnected(Wifi& wifi, Prefs& net_prefs, uint32_t log_every_ms,
                      uint32_t max_terminal_strikes) {
  ESP_LOGI(kTag, "waiting for STA IP …");
  uint32_t waited_ms = 0;
  while (wifi.state() != Wifi::State::kConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    waited_ms += 1000;
    if (log_every_ms > 0 && waited_ms % log_every_ms == 0) {
      ESP_LOGI(kTag, "still no IP after %u s",
               static_cast<unsigned>(waited_ms / 1000));
    }
    if (max_terminal_strikes > 0 &&
        wifi.consecutive_terminal_disconnects() >= max_terminal_strikes) {
      // Stored creds are rejected by the AP and no amount of retrying
      // will fix it. Wipe the SSID so the next boot enters provisioning
      // mode, and reboot. We keep the password pref untouched — it will
      // be overwritten on the next successful provisioning submit.
      ESP_LOGE(kTag,
               "%u terminal STA disconnects (reason=%u); clearing net/ssid "
               "and rebooting into provisioning",
               static_cast<unsigned>(max_terminal_strikes),
               static_cast<unsigned>(wifi.last_disconnect_reason()));
      net_prefs.SetString("ssid", "");
      net_prefs.Commit();
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }
  }
  ESP_LOGI(kTag, "STA up, ip=%s", wifi.ip().c_str());
}

OutageWatchdog::OutageWatchdog(uint32_t outage_reboot_minutes)
    : outage_reboot_minutes_(outage_reboot_minutes) {}

void OutageWatchdog::Tick(Wifi& wifi, uint32_t now_ms) {
  const bool connected = (wifi.state() == Wifi::State::kConnected);
  if (connected) {
    // GOT_IP (and every tick thereafter) clears the outage stamp — the
    // watchdog only fires on *continuous* disconnect, not cumulative.
    disconnected_since_ms_ = 0;
    was_connected_ = true;
    return;
  }
  // First tick after a connected→disconnected edge: stamp the start.
  // We gate on was_connected_ so the very first post-boot ticks (while
  // the initial association is still in flight) don't arm the timer —
  // WaitForConnected + the strikes path own that window.
  if (was_connected_ && disconnected_since_ms_ == 0) {
    disconnected_since_ms_ = now_ms;
    ESP_LOGW(kTag, "STA disconnected; outage reboot timer armed (%u min)",
             static_cast<unsigned>(outage_reboot_minutes_));
  }
  if (ShouldOutageReboot(disconnected_since_ms_, now_ms,
                         outage_reboot_minutes_)) {
    ESP_LOGE(kTag, "wifi outage reboot after %u min continuous disconnect",
             static_cast<unsigned>(outage_reboot_minutes_));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
}

}  // namespace btclock
