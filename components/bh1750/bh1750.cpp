#include "bh1750.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "bh1750";
}  // namespace

Bh1750::Bh1750(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz)
    : bus_(&bus) {
  dev_ = bus.AddDevice(addr_7bit, scl_hz);
  ESP_LOGI(kTag, "attached addr=0x%02X", addr_7bit);
}

Bh1750::~Bh1750() {
  if (dev_ != nullptr) {
    i2c_master_bus_rm_device(dev_);
  }
}

esp_err_t Bh1750::Begin(Mode mode) {
  const uint8_t cmd = static_cast<uint8_t>(mode);
  return i2c_master_transmit(dev_, &cmd, 1, 50);
}

float Bh1750::ReadLux() {
  uint8_t raw[2] = {};
  esp_err_t err = i2c_master_receive(dev_, raw, sizeof(raw), 50);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "read err=%s", esp_err_to_name(err));
    return -1.0f;
  }
  const uint16_t counts = (static_cast<uint16_t>(raw[0]) << 8) | raw[1];
  // Per datasheet: lx = counts / 1.2.
  return static_cast<float>(counts) / 1.2f;
}

}  // namespace btclock
