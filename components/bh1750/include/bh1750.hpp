#pragma once

#include <cstdint>

#include "bh1750_math.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_bus.hpp"

namespace btclock {

// BH1750 ambient light sensor, I2C, 0x23 or 0x5C depending on ADDR pin.
class Bh1750 {
 public:
  enum class Mode : uint8_t {
    // POWER_ON opcode is sent before any measurement mode — the datasheet
    // requires it when coming out of POWER_DOWN (the post-reset default).
    kPowerOn = 0x01,
    kContinuousHighRes = 0x10,  // 1 lx resolution, 120 ms
    kContinuousHighRes2 = 0x11, // 0.5 lx resolution, 120 ms
    kOneshotHighRes = 0x20,
  };

  Bh1750(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz = 400'000);
  ~Bh1750();

  Bh1750(const Bh1750&) = delete;
  Bh1750& operator=(const Bh1750&) = delete;

  // Probe the chip then send POWER_ON + the requested measurement mode.
  // Returns ESP_ERR_NOT_FOUND if the sensor does not ACK — lets the
  // manager degrade gracefully on boards where the part was depopulated
  // or wired but not soldered. IsInitialized() tracks the outcome.
  esp_err_t Init(Mode mode = Mode::kContinuousHighRes);

  // Legacy entry point — equivalent to Init() minus the probe step.
  // Retained so existing callers keep working; new code should call
  // Init() so the "no sensor present" case doesn't crash on bus errors.
  esp_err_t Begin(Mode mode = Mode::kContinuousHighRes);

  bool IsInitialized() const { return initialised_; }

  // Blocking read. Returns a negative value if the sensor was never
  // initialised or the I2C transaction failed (bus glitch, disconnect).
  float ReadLux();

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
  uint16_t addr_7bit_ = 0;
  uint32_t scl_hz_ = 400'000;
  bool initialised_ = false;
};

}  // namespace btclock
