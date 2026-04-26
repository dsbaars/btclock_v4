#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.hpp"

namespace btclock {

// Thin driver for the Microchip MCP23017 16-bit I2C I/O expander.
// IOCON.BANK=0 register layout assumed (the hardware default).
//
// Thread safety: every public method takes a recursive mutex before
// any I2C transaction. Needed because (a) ReadPin calls ReadPort, so
// nested public-entry is a real pattern within this driver, and (b)
// multiple tasks hit the MCP — EPD driver on the render task, button
// scan on the event-loop task, frontlight control on the ambient
// task, and settings worker. Concurrent I2C transactions against the
// same device handle corrupt the read-modify-write sequence on IODIR
// / GPPU / GPIO, which the SSD1680 panels see as CS/RST glitches and
// partial-refresh flakiness.
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
  // writes the full port.
  esp_err_t WritePin(uint8_t pin, bool high);

 private:
  esp_err_t WriteReg(uint8_t reg, uint8_t val);
  esp_err_t WriteReg16(uint8_t reg, uint16_t val);  // writes reg + reg+1
  esp_err_t ReadReg16(uint8_t reg, uint16_t* val);

  // RAII scoped lock over mutex_ (recursive — a public entry point can
  // call a sibling public entry point without deadlocking).
  class Lock {
   public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) {
      if (m_ != nullptr) xSemaphoreTakeRecursive(m_, portMAX_DELAY);
    }
    ~Lock() {
      if (m_ != nullptr) xSemaphoreGiveRecursive(m_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    SemaphoreHandle_t m_;
  };

  i2c_master_dev_handle_t dev_ = nullptr;
  I2cBus* bus_ = nullptr;
  uint16_t output_cache_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace btclock
