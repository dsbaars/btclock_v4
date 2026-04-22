#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_bus.hpp"

namespace btclock {

// BH1750 ambient light sensor, I2C, 0x23 or 0x5C depending on ADDR pin.
class Bh1750 {
 public:
  enum class Mode : uint8_t {
    kContinuousHighRes = 0x10,  // 1 lx resolution, 120 ms
    kContinuousHighRes2 = 0x11, // 0.5 lx resolution, 120 ms
    kOneshotHighRes = 0x20,
  };

  Bh1750(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz = 400'000);
  ~Bh1750();

  Bh1750(const Bh1750&) = delete;
  Bh1750& operator=(const Bh1750&) = delete;

  esp_err_t Begin(Mode mode = Mode::kContinuousHighRes);

  // Returns lux. Returns a negative value on read error.
  float ReadLux();

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
};

}  // namespace btclock
