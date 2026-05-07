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
//
// NOT thread-safe. The frontlight task is the sole owner of all
// I2C-touching methods (Begin / SetDuty / SetAll). SetOutputEnable
// only flips a GPIO pin — also single-caller in practice. If a second
// task ever needs to interleave PWM writes, add a mutex; concurrent
// SetDuty calls would split the 5-byte auto-increment transaction
// and glitch the LED outputs at the boundary.
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
  // Writes ON_L/ON_H/OFF_L/OFF_H starting at base_reg as a single
  // 5-byte transaction (relies on MODE1.AI=1). Handles full-on /
  // full-off encoding at duty extremes. Used for both per-channel
  // (LED0_ON_L + 4*ch) and broadcast (ALL_LED_ON_L) writes.
  esp_err_t WriteLedQuad(uint8_t base_reg, uint16_t duty);

  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
  gpio_num_t oe_gpio_ = GPIO_NUM_NC;
};

}  // namespace btclock
