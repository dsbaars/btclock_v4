// Variant-gated TU — see light_sensor.hpp's `#if BTCLOCK_HAS_BH1750`.
#if BTCLOCK_HAS_BH1750

#include "io/light_sensor.hpp"

#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "light_sensor";
}  // namespace

LightSensor::LightSensor(Bh1750& sensor, Config cfg)
    : sensor_(sensor), cfg_(cfg), available_(sensor.IsInitialized()) {
  if (!available_) {
    ESP_LOGW(kTag, "sensor not initialised — GetLux() will return -1");
  }
}

LightSensor::~LightSensor() {
  stop_.store(true, std::memory_order_relaxed);
  if (task_ != nullptr) {
    // Best-effort: signal and wait briefly. The poll task checks stop_
    // between sleeps so a clean shutdown happens within one poll cycle.
    for (int i = 0; i < 10 && eTaskGetState(task_) != eDeleted; ++i) {
      vTaskDelay(pdMS_TO_TICKS(cfg_.poll_ms / 5 + 20));
    }
  }
}

void LightSensor::Start() {
  if (!available_ || task_ != nullptr) return;
  xTaskCreate(&LightSensor::TaskTrampoline, "bh1750_poll", cfg_.task_stack,
              this, static_cast<UBaseType_t>(cfg_.task_priority), &task_);
}

void LightSensor::TaskTrampoline(void* arg) {
  static_cast<LightSensor*>(arg)->Run();
}

void LightSensor::Run() {
  // First measurement in continuous-H-res mode is valid after ~180 ms.
  // Sleep a generous 200 ms so the first reading is meaningful.
  vTaskDelay(pdMS_TO_TICKS(200));
  while (!stop_.load(std::memory_order_relaxed)) {
    const float lux = sensor_.ReadLux();
    if (lux >= 0.0f) {
      lux_.store(lux, std::memory_order_relaxed);
    }
    vTaskDelay(pdMS_TO_TICKS(cfg_.poll_ms));
  }
  TaskHandle_t me = task_;
  task_ = nullptr;
  vTaskDelete(me);
}

}  // namespace btclock

#endif  // BTCLOCK_HAS_BH1750
