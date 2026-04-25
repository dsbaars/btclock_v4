// Low-level SPI + DC/CS/BUSY/RESET wrappers. No IC-specific knowledge
// lives here — the SSD1680 / UC8179 driver bases sit on top.

#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "mcp23017.hpp"

namespace btclock {

// A single digital I/O line, backed by either a native MCU GPIO or
// an MCP23017 expander pin. V8 uses both; Rev A/B always native.
// Native writes are single-cycle register hits; MCP writes carry
// ~1 ms of I2C per toggle, so callers that poll (BUSY) should slow
// down when is_mcp() is true.
class EpdIoPin {
 public:
  EpdIoPin() = default;

  static EpdIoPin Native(gpio_num_t pin) {
    EpdIoPin p;
    p.kind_ = Kind::kNative;
    p.native_ = pin;
    return p;
  }
  static EpdIoPin Mcp(Mcp23017* mcp, uint8_t pin) {
    EpdIoPin p;
    p.kind_ = Kind::kMcp;
    p.mcp_ = mcp;
    p.mcp_pin_ = pin;
    return p;
  }

  bool is_native() const { return kind_ == Kind::kNative; }
  bool is_mcp() const { return kind_ == Kind::kMcp; }

  // Runtime debug string, e.g. "GPIO14" or "mcp.8". Returns a pointer
  // to a per-call static buffer; don't hold across calls.
  const char* Describe() const;

  esp_err_t ConfigureAsOutput(bool initial_high);
  esp_err_t ConfigureAsInput();

  esp_err_t Write(bool high);
  bool Read() const;

 private:
  enum class Kind : uint8_t { kNative, kMcp };
  Kind kind_ = Kind::kNative;
  gpio_num_t native_ = GPIO_NUM_NC;
  Mcp23017* mcp_ = nullptr;
  uint8_t mcp_pin_ = 0;
};

class EpdBus {
 public:
  EpdBus(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi,
         gpio_num_t dc, uint32_t clk_hz = 4 * 1000 * 1000,
         int max_transfer_bytes = 8 * 1024);
  ~EpdBus();

  EpdBus(const EpdBus&) = delete;
  EpdBus& operator=(const EpdBus&) = delete;

  esp_err_t SendCommand(EpdIoPin& cs, uint8_t cmd,
                        const uint8_t* params = nullptr,
                        size_t nparams = 0);
  esp_err_t SendData(EpdIoPin& cs, const uint8_t* data, size_t len);

 private:
  spi_host_device_t host_;
  spi_device_handle_t dev_ = nullptr;
  gpio_num_t dc_;
};

}  // namespace btclock
