#include "mcp23017.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "mcp23017";

// IOCON.BANK=0 register map.
constexpr uint8_t kRegIodirA = 0x00;
constexpr uint8_t kRegIocon = 0x0A;
constexpr uint8_t kRegGppuA = 0x0C;
constexpr uint8_t kRegGpioA = 0x12;

// IOCON value we expect: BANK=0 (sequential A/B layout), SEQOP=0
// (auto-increment so a single transaction writes both halves of a
// 16-bit register pair), all other bits zero. This matches the
// MCP23017 power-on default; we write it explicitly so a transient
// bus glitch flipping BANK or SEQOP doesn't silently break our
// 16-bit reads/writes (with BANK=1, IODIRB jumps from 0x01 to 0x10
// and the auto-increment trick reads garbage).
constexpr uint8_t kIoconBank0Seqon = 0x00;

// Post-direction-change settle window. An MCP23017 IODIR write takes
// effect on the next ACK but the driven pin level can slew for a few
// hundred microseconds while internal pull paths equalise. The SSD1680
// panels use active-low CS/RST; a brief indeterminate level during the
// first SPI edge after a direction flip manifests as dropped commands
// and stale VRAM. 1 ms is cheap (direction flips are infrequent — only
// during EpdIoPin::ConfigureAs*) and comfortably longer than the slew.
constexpr TickType_t kDirectionSettleMs = 1;
}  // namespace

Mcp23017::Mcp23017(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz)
    : bus_(&bus) {
  dev_ = bus.AddDevice(addr_7bit, scl_hz);
  mutex_ = xSemaphoreCreateRecursiveMutex();
  if (mutex_ == nullptr) {
    ESP_LOGE(kTag, "recursive mutex alloc failed addr=0x%02X", addr_7bit);
  }
  esp_err_t err = WriteReg(kRegIocon, kIoconBank0Seqon);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "iocon init failed addr=0x%02X err=%d", addr_7bit, err);
  }
  ESP_LOGI(kTag, "attached addr=0x%02X scl=%u", addr_7bit,
           static_cast<unsigned>(scl_hz));
}

Mcp23017::~Mcp23017() {
  if (dev_ != nullptr) {
    i2c_master_bus_rm_device(dev_);
  }
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
  }
}

esp_err_t Mcp23017::WriteReg(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev_, buf, 2, 50);
}

esp_err_t Mcp23017::WriteReg16(uint8_t reg, uint16_t val) {
  const uint8_t buf[3] = {reg, static_cast<uint8_t>(val & 0xFF),
                          static_cast<uint8_t>(val >> 8)};
  return i2c_master_transmit(dev_, buf, 3, 50);
}

esp_err_t Mcp23017::ReadReg16(uint8_t reg, uint16_t* val) {
  uint8_t out[2] = {};
  esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, out, 2, 50);
  if (err == ESP_OK) {
    *val = static_cast<uint16_t>(out[0]) | (static_cast<uint16_t>(out[1]) << 8);
  }
  return err;
}

esp_err_t Mcp23017::SetDirectionPort(uint16_t pin_mask_inputs) {
  Lock lk(mutex_);
  // IODIR: 1 = input, 0 = output.
  esp_err_t err = WriteReg16(kRegIodirA, pin_mask_inputs);
  if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(kDirectionSettleMs));
  return err;
}

esp_err_t Mcp23017::SetDirection(uint8_t pin, PinMode mode) {
  Lock lk(mutex_);
  // Single-pin path is convenient for tests; reads-modify-writes IODIR/GPPU.
  uint16_t iodir = 0;
  ESP_RETURN_ON_ERROR(ReadReg16(kRegIodirA, &iodir), kTag, "read iodir");
  uint16_t gppu = 0;
  ESP_RETURN_ON_ERROR(ReadReg16(kRegGppuA, &gppu), kTag, "read gppu");
  const uint16_t bit = static_cast<uint16_t>(1U << pin);
  if (mode == PinMode::kInputPullup) {
    iodir |= bit;
    gppu |= bit;
  } else {
    iodir &= ~bit;
    gppu &= ~bit;
  }
  ESP_RETURN_ON_ERROR(WriteReg16(kRegIodirA, iodir), kTag, "write iodir");
  ESP_RETURN_ON_ERROR(WriteReg16(kRegGppuA, gppu), kTag, "write gppu");
  vTaskDelay(pdMS_TO_TICKS(kDirectionSettleMs));
  return ESP_OK;
}

esp_err_t Mcp23017::WritePort(uint16_t value) {
  Lock lk(mutex_);
  output_cache_ = value;
  return WriteReg16(kRegGpioA, value);
}

esp_err_t Mcp23017::WritePin(uint8_t pin, bool high) {
  Lock lk(mutex_);
  const uint16_t bit = static_cast<uint16_t>(1U << pin);
  if (high)
    output_cache_ |= bit;
  else
    output_cache_ &= ~bit;
  return WriteReg16(kRegGpioA, output_cache_);
}

esp_err_t Mcp23017::ReadPort(uint16_t* value) {
  Lock lk(mutex_);
  return ReadReg16(kRegGpioA, value);
}

bool Mcp23017::ReadPin(uint8_t pin) {
  // Lock held across the sibling ReadPort call — recursive mutex means
  // the inner take is cheap and doesn't deadlock.
  Lock lk(mutex_);
  uint16_t port = 0;
  if (ReadPort(&port) != ESP_OK) return false;
  return (port >> pin) & 1U;
}

}  // namespace btclock
