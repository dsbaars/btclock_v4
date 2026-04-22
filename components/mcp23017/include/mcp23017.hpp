#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_bus.hpp"

namespace btclock {

// Thin driver for the Microchip MCP23017 16-bit I2C I/O expander.
// IOCON.BANK=0 register layout assumed (the hardware default).
class Mcp23017 {
 public:
  // pin_mask: bit 0 = pin 0 (GPA0) ... bit 15 = pin 15 (GPB7).
  enum class PinMode : uint8_t { kOutput, kInputPullup };

  Mcp23017(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz = 400'000);
  ~Mcp23017();

  Mcp23017(const Mcp23017&) = delete;
  Mcp23017& operator=(const Mcp23017&) = delete;

  esp_err_t SetDirection(uint8_t pin, PinMode mode);
  esp_err_t SetDirectionPort(uint16_t pin_mask_inputs);
  esp_err_t WritePort(uint16_t value);
  esp_err_t ReadPort(uint16_t* value);
  bool ReadPin(uint8_t pin);

  // Cache-aware single-pin write: updates the cached output latch and
  // writes the full port. Thread safety is the caller's responsibility.
  esp_err_t WritePin(uint8_t pin, bool high);

 private:
  esp_err_t WriteReg(uint8_t reg, uint8_t val);
  esp_err_t WriteReg16(uint8_t reg, uint16_t val);  // writes reg + reg+1
  esp_err_t ReadReg16(uint8_t reg, uint16_t* val);

  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
  uint16_t output_cache_ = 0;
};

}  // namespace btclock
