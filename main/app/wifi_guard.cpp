#include "app/wifi_guard.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "wifi-guard";
}  // namespace

void WaitForConnected(Wifi& wifi, uint32_t log_every_ms) {
  ESP_LOGI(kTag, "waiting for STA IP …");
  uint32_t waited_ms = 0;
  while (wifi.state() != Wifi::State::kConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    waited_ms += 1000;
    if (log_every_ms > 0 && waited_ms % log_every_ms == 0) {
      ESP_LOGI(kTag, "still no IP after %u s",
               static_cast<unsigned>(waited_ms / 1000));
    }
  }
  ESP_LOGI(kTag, "STA up, ip=%s", wifi.ip().c_str());
}

}  // namespace btclock
