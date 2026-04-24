// Ambient-light sensor manager.
//
// Wraps a BH1750 behind a low-priority poll task so request handlers
// (e.g. /api/settings' hasLightLevel/lightLevel emission) can read the
// latest lux without blocking on I2C. Boards without the sensor skip
// construction entirely — nullptr flows through the webserver's
// LightSensorIface and the response fields stay absent.

#pragma once

#include <atomic>
#include <cstdint>

#include "bh1750.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {

struct LightSensorConfig {
  // Poll period, ms. 2 s matches the old firmware's ambient-sampling
  // cadence and is slow enough to keep the sensor in continuous-mode
  // without spinning the I2C bus.
  uint32_t poll_ms = 2000;
  // Task tuning. Stack at 2 KiB is plenty for two I2C transactions +
  // ESP-LOG; priority 1 keeps us below UI + networking tasks.
  uint32_t task_stack = 2048;
  uint32_t task_priority = 1;
};

class LightSensor {
 public:
  using Config = LightSensorConfig;

  // `sensor` must outlive this manager. If Init() on the sensor failed
  // the manager stays in "unavailable" mode and never starts the task.
  explicit LightSensor(Bh1750& sensor, Config cfg = {});
  ~LightSensor();

  LightSensor(const LightSensor&) = delete;
  LightSensor& operator=(const LightSensor&) = delete;

  // Spawn the poll task. Idempotent. No-op if the sensor never
  // initialised (IsAvailable() == false).
  void Start();

  // Returns false when the sensor wasn't detected at boot; handlers
  // use this to suppress the lightLevel/hasLightLevel JSON fields.
  bool IsAvailable() const { return available_; }

  // Most recent lux sample, or <0 before the first successful read.
  float GetLux() const { return lux_.load(std::memory_order_relaxed); }

 private:
  static void TaskTrampoline(void* arg);
  void Run();

  Bh1750& sensor_;
  Config cfg_;
  bool available_ = false;
  std::atomic<float> lux_{-1.0f};
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_{false};
};

}  // namespace btclock
