#include "epd/bus.hpp"

#include <cstdio>

#include "esp_check.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "epd-bus";

void ConfigureNativeOutput(gpio_num_t pin, int level) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << static_cast<int>(pin);
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  gpio_set_level(pin, level);
}

void ConfigureNativeInput(gpio_num_t pin) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << static_cast<int>(pin);
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
}

}  // namespace

const char* EpdIoPin::Describe() const {
  // Rotating small buffer pool so multiple Describe() calls in the
  // same printf statement don't clobber one another. Single-task
  // caller (EpdPanel boot logs), so no thread-safety story needed.
  static char bufs[4][16];
  static unsigned idx = 0;
  char* b = bufs[idx++ & 3];
  if (kind_ == Kind::kMcp) {
    std::snprintf(b, 16, "mcp.%u", static_cast<unsigned>(mcp_pin_));
  } else {
    std::snprintf(b, 16, "GPIO%d", static_cast<int>(native_));
  }
  return b;
}

esp_err_t EpdIoPin::ConfigureAsOutput(bool initial_high) {
  if (kind_ == Kind::kMcp) {
    if (mcp_ == nullptr) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(
        mcp_->SetDirection(mcp_pin_, Mcp23017::PinMode::kOutput),
        kTag, "mcp.dir.out");
    return mcp_->WritePin(mcp_pin_, initial_high);
  }
  ConfigureNativeOutput(native_, initial_high ? 1 : 0);
  return ESP_OK;
}

esp_err_t EpdIoPin::ConfigureAsInput() {
  if (kind_ == Kind::kMcp) {
    if (mcp_ == nullptr) return ESP_ERR_INVALID_STATE;
    return mcp_->SetDirection(mcp_pin_, Mcp23017::PinMode::kInputPullup);
  }
  ConfigureNativeInput(native_);
  return ESP_OK;
}

esp_err_t EpdIoPin::Write(bool high) {
  if (kind_ == Kind::kMcp) {
    if (mcp_ == nullptr) return ESP_ERR_INVALID_STATE;
    return mcp_->WritePin(mcp_pin_, high);
  }
  gpio_set_level(native_, high ? 1 : 0);
  return ESP_OK;
}

bool EpdIoPin::Read() const {
  if (kind_ == Kind::kMcp) {
    if (mcp_ == nullptr) return false;
    return mcp_->ReadPin(mcp_pin_);
  }
  return gpio_get_level(native_) != 0;
}

EpdBus::EpdBus(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi,
               gpio_num_t dc, uint32_t clk_hz, int max_transfer_bytes)
    : host_(host), dc_(dc) {
  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = mosi;
  bus_cfg.miso_io_num = -1;
  bus_cfg.sclk_io_num = sclk;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = max_transfer_bytes;
  ESP_ERROR_CHECK(spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.clock_speed_hz = clk_hz;
  dev_cfg.mode = 0;
  dev_cfg.spics_io_num = -1;
  dev_cfg.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(host, &dev_cfg, &dev_));

  ConfigureNativeOutput(dc_, 1);
  ESP_LOGI(kTag, "bus up host=%d sclk=GPIO%d mosi=GPIO%d dc=GPIO%d clk=%uHz",
           host, static_cast<int>(sclk), static_cast<int>(mosi),
           static_cast<int>(dc_), static_cast<unsigned>(clk_hz));
}

EpdBus::~EpdBus() {
  if (dev_ != nullptr) spi_bus_remove_device(dev_);
  spi_bus_free(host_);
}

esp_err_t EpdBus::SendCommand(EpdIoPin& cs, uint8_t cmd,
                              const uint8_t* params, size_t nparams) {
  spi_transaction_t t = {};
  cs.Write(false);
  gpio_set_level(dc_, 0);
  t.length = 8;
  t.tx_buffer = &cmd;
  esp_err_t err = spi_device_polling_transmit(dev_, &t);
  if (err != ESP_OK) {
    cs.Write(true);
    return err;
  }
  if (nparams > 0) {
    gpio_set_level(dc_, 1);
    spi_transaction_t td = {};
    td.length = 8 * nparams;
    td.tx_buffer = params;
    err = spi_device_polling_transmit(dev_, &td);
  }
  cs.Write(true);
  return err;
}

esp_err_t EpdBus::SendData(EpdIoPin& cs, const uint8_t* data, size_t len) {
  spi_transaction_t t = {};
  cs.Write(false);
  gpio_set_level(dc_, 1);
  t.length = 8 * len;
  t.tx_buffer = data;
  esp_err_t err = spi_device_polling_transmit(dev_, &t);
  cs.Write(true);
  return err;
}

}  // namespace btclock
