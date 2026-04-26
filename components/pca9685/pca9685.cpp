#include "pca9685.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "pca9685";

constexpr uint8_t kRegMode1 = 0x00;
constexpr uint8_t kRegMode2 = 0x01;
constexpr uint8_t kRegLed0OnL = 0x06;
constexpr uint8_t kRegPreScale = 0xFE;

// MODE1 bits
constexpr uint8_t kMode1Restart = 0x80;
constexpr uint8_t kMode1ExtClk = 0x40;
constexpr uint8_t kMode1Ai = 0x20;  // auto-increment
constexpr uint8_t kMode1Sleep = 0x10;
constexpr uint8_t kMode1AllCall = 0x01;
// MODE2 bits
constexpr uint8_t kMode2OutDrv = 0x04;  // totem-pole
}  // namespace

Pca9685::Pca9685(I2cBus& bus, uint16_t addr_7bit, gpio_num_t output_enable_gpio,
                 uint32_t scl_hz)
    : bus_(&bus), oe_gpio_(output_enable_gpio) {
  dev_ = bus.AddDevice(addr_7bit, scl_hz);
  if (oe_gpio_ != GPIO_NUM_NC) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << static_cast<int>(oe_gpio_);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(oe_gpio_, 1);  // start with outputs disabled
  }
  ESP_LOGI(kTag, "attached addr=0x%02X oe_gpio=GPIO%d", addr_7bit,
           static_cast<int>(oe_gpio_));
}

Pca9685::~Pca9685() {
  if (dev_ != nullptr) {
    i2c_master_bus_rm_device(dev_);
  }
}

esp_err_t Pca9685::WriteReg(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev_, buf, 2, 50);
}

esp_err_t Pca9685::Begin(uint32_t pwm_hz) {
  // Datasheet formula: PRE_SCALE = round(25 MHz / (4096 * freq)) - 1.
  uint32_t prescale_val = 25'000'000U / (4096U * pwm_hz);
  if (prescale_val > 0) --prescale_val;
  if (prescale_val < 3) prescale_val = 3;
  if (prescale_val > 255) prescale_val = 255;

  ESP_RETURN_ON_ERROR(WriteReg(kRegMode1, kMode1Sleep | kMode1AllCall), kTag,
                      "mode1 sleep");
  ESP_RETURN_ON_ERROR(
      WriteReg(kRegPreScale, static_cast<uint8_t>(prescale_val)), kTag,
      "prescale");
  ESP_RETURN_ON_ERROR(WriteReg(kRegMode1, kMode1Ai | kMode1AllCall), kTag,
                      "mode1 wake");
  vTaskDelay(pdMS_TO_TICKS(1));
  ESP_RETURN_ON_ERROR(
      WriteReg(kRegMode1, kMode1Restart | kMode1Ai | kMode1AllCall), kTag,
      "mode1 restart");
  ESP_RETURN_ON_ERROR(WriteReg(kRegMode2, kMode2OutDrv), kTag, "mode2");
  ESP_LOGI(kTag, "begin ok pwm_hz=%u prescale=0x%02X",
           static_cast<unsigned>(pwm_hz), static_cast<unsigned>(prescale_val));
  return ESP_OK;
}

void Pca9685::SetOutputEnable(bool on) {
  if (oe_gpio_ != GPIO_NUM_NC) {
    gpio_set_level(oe_gpio_, on ? 0 : 1);  // active low
  }
}

esp_err_t Pca9685::SetDuty(uint8_t channel, uint16_t duty) {
  if (channel > 15) return ESP_ERR_INVALID_ARG;
  if (duty > 4095) duty = 4095;

  const uint8_t reg = kRegLed0OnL + 4 * channel;
  // ON=0, OFF=duty (when duty==4095 set the full-on bit; 0 → full off)
  uint16_t on = 0;
  uint16_t off = duty;
  if (duty == 4095) {
    on = 0x1000;
    off = 0;
  } else if (duty == 0) {
    on = 0;
    off = 0x1000;
  }

  const uint8_t payload[5] = {
      reg,
      static_cast<uint8_t>(on & 0xFF),
      static_cast<uint8_t>(on >> 8),
      static_cast<uint8_t>(off & 0xFF),
      static_cast<uint8_t>(off >> 8),
  };
  return i2c_master_transmit(dev_, payload, sizeof(payload), 50);
}

esp_err_t Pca9685::SetAll(uint16_t duty) {
  for (uint8_t ch = 0; ch < 16; ++ch) {
    esp_err_t err = SetDuty(ch, duty);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

}  // namespace btclock
