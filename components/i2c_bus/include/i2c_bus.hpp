#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hal/gpio_types.h"

namespace btclock {

// Thin RAII wrapper around a single I2C master bus.
class I2cBus {
 public:
  I2cBus(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl);
  ~I2cBus();

  I2cBus(const I2cBus&) = delete;
  I2cBus& operator=(const I2cBus&) = delete;

  i2c_master_bus_handle_t handle() const { return bus_; }

  // Add a device endpoint. Caller owns the returned handle and must remove
  // it before the bus is destroyed.
  i2c_master_dev_handle_t AddDevice(uint16_t addr_7bit, uint32_t scl_hz);

  // Probe an address; returns true if a slave ACKs.
  bool Probe(uint16_t addr_7bit, uint32_t timeout_ms = 50) const;

  // Scan the 7-bit address space [0x03, 0x77] and log all responders.
  void ScanAndLog() const;

 private:
  i2c_master_bus_handle_t bus_ = nullptr;
};

}  // namespace btclock
