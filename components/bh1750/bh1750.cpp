#include "bh1750.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "bh1750";
}  // namespace

Bh1750::Bh1750(I2cBus& bus, uint16_t addr_7bit, uint32_t scl_hz)
    : bus_(&bus), addr_7bit_(addr_7bit), scl_hz_(scl_hz) {
  dev_ = bus.AddDevice(addr_7bit, scl_hz);
  ESP_LOGI(kTag, "attached addr=0x%02X", addr_7bit);
}

Bh1750::~Bh1750() {
  if (dev_ != nullptr) {
    i2c_master_bus_rm_device(dev_);
  }
}

esp_err_t Bh1750::Init(Mode mode) {
  // Probe first so a missing chip reports a clean ESP_ERR_NOT_FOUND
  // instead of a bus-level NACK that the caller would otherwise have
  // to translate itself.
  //
  // ADDR-pin fallback: production Rev B boards tie ADDR high (0x5C),
  // but field units assembled with the ADDR pad left floating or tied
  // low present at 0x23. Probe the configured address first, then the
  // alternate. On success at the alternate we re-point `dev_` so reads
  // land at the right device. This keeps the pin map declarative
  // (kBhAddr = 0x5C) while forgiving a wiring variance.
  if (bus_ == nullptr) {
    initialised_ = false;
    return ESP_ERR_NOT_FOUND;
  }
  if (!bus_->Probe(addr_7bit_)) {
    const uint16_t alt_addr =
        (addr_7bit_ == 0x5C) ? static_cast<uint16_t>(0x23)
                             : static_cast<uint16_t>(0x5C);
    if (bus_->Probe(alt_addr)) {
      ESP_LOGW(kTag, "no device at 0x%02X, found at 0x%02X — using alt",
               addr_7bit_, alt_addr);
      if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
      }
      addr_7bit_ = alt_addr;
      dev_ = bus_->AddDevice(alt_addr, scl_hz_);
    } else {
      ESP_LOGW(kTag, "no device at 0x%02X (alt 0x%02X also absent)",
               addr_7bit_, alt_addr);
      initialised_ = false;
      return ESP_ERR_NOT_FOUND;
    }
  }
  const uint8_t power_on = static_cast<uint8_t>(Mode::kPowerOn);
  esp_err_t err = i2c_master_transmit(dev_, &power_on, 1, 50);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "power_on err=%s", esp_err_to_name(err));
    initialised_ = false;
    return err;
  }
  const uint8_t op = static_cast<uint8_t>(mode);
  err = i2c_master_transmit(dev_, &op, 1, 50);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "mode err=%s", esp_err_to_name(err));
    initialised_ = false;
    return err;
  }
  initialised_ = true;
  return ESP_OK;
}

esp_err_t Bh1750::Begin(Mode mode) {
  const uint8_t cmd = static_cast<uint8_t>(mode);
  const esp_err_t err = i2c_master_transmit(dev_, &cmd, 1, 50);
  initialised_ = (err == ESP_OK);
  return err;
}

float Bh1750::ReadLux() {
  if (!initialised_) return -1.0f;
  uint8_t raw[2] = {};
  esp_err_t err = i2c_master_receive(dev_, raw, sizeof(raw), 50);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "read err=%s", esp_err_to_name(err));
    return -1.0f;
  }
  const uint16_t counts = (static_cast<uint16_t>(raw[0]) << 8) | raw[1];
  return bh1750::RawToLux(counts);
}

}  // namespace btclock
