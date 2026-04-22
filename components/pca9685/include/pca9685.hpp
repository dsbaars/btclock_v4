#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_bus.hpp"

namespace btclock {

// Driver for the NXP PCA9685 16-channel 12-bit I2C PWM controller.
// Mirrors only the subset of behaviour the frontlight uses:
//   - SLEEP -> set PRE_SCALE -> wake with auto-increment
//   - per-channel SetDuty(chan, 0..4095)
//   - optional output-enable GPIO (active-low) for the whole chip
class Pca9685 {
 public:
  Pca9685(I2cBus& bus, uint16_t addr_7bit, gpio_num_t output_enable_gpio,
          uint32_t scl_hz = 400'000);
  ~Pca9685();

  Pca9685(const Pca9685&) = delete;
  Pca9685& operator=(const Pca9685&) = delete;

  // Must be called once before SetDuty. Sets mode registers + PWM frequency.
  esp_err_t Begin(uint32_t pwm_hz = 1000);

  // Drive OE pin low (output enabled) or high (all channels Hi-Z).
  void SetOutputEnable(bool on);

  // Channel 0..15, duty 0..4095 (12-bit).
  esp_err_t SetDuty(uint8_t channel, uint16_t duty);

  // Drive all 16 channels to the same duty.
  esp_err_t SetAll(uint16_t duty);

 private:
  esp_err_t WriteReg(uint8_t reg, uint8_t val);

  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
  gpio_num_t oe_gpio_ = GPIO_NUM_NC;
};

}  // namespace btclock
