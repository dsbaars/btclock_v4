#include "i2c_bus.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "i2c_bus";
}  // namespace

I2cBus::I2cBus(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl) {
  i2c_master_bus_config_t cfg = {};
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port = port;
  cfg.sda_io_num = sda;
  cfg.scl_io_num = scl;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus_));
  ESP_LOGI(kTag, "bus up port=%d sda=GPIO%d scl=GPIO%d", port,
           static_cast<int>(sda), static_cast<int>(scl));
}

I2cBus::~I2cBus() {
  if (bus_ != nullptr) {
    i2c_del_master_bus(bus_);
  }
}

i2c_master_dev_handle_t I2cBus::AddDevice(uint16_t addr_7bit, uint32_t scl_hz) {
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr_7bit;
  cfg.scl_speed_hz = scl_hz;
  i2c_master_dev_handle_t dev = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_, &cfg, &dev));
  return dev;
}

bool I2cBus::Probe(uint16_t addr_7bit, uint32_t timeout_ms) const {
  return i2c_master_probe(bus_, addr_7bit, timeout_ms) == ESP_OK;
}

void I2cBus::ScanAndLog() const {
  ESP_LOGI(kTag, "scanning 0x03..0x77");
  int found = 0;
  for (uint16_t a = 0x03; a <= 0x77; ++a) {
    if (Probe(a, 20)) {
      ESP_LOGI(kTag, "  0x%02X ACKed", a);
      ++found;
    }
  }
  ESP_LOGI(kTag, "scan done, %d device(s)", found);
}

}  // namespace btclock
