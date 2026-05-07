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
// 0xFA..0xFD: writing to ALL_LED_* broadcasts to all 16 channels in
// one 5-byte auto-increment transaction. ~16x faster than looping
// per-channel for boot-time SetAll.
constexpr uint8_t kRegAllLedOnL = 0xFA;
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
  if (dev_ == nullptr) return ESP_ERR_INVALID_STATE;
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev_, buf, 2, 50);
}

esp_err_t Pca9685::WriteLedQuad(uint8_t base_reg, uint16_t duty) {
  if (dev_ == nullptr) return ESP_ERR_INVALID_STATE;
  if (duty > 4095) duty = 4095;
  // Datasheet § 7.3.3: at the duty extremes, set the FULL_ON or
  // FULL_OFF bit (bit 12 of the high byte) instead of relying on the
  // PWM counter to never tick — counter-only encoding leaves a
  // single-cycle non-driven slice each PWM period.
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
      base_reg,
      static_cast<uint8_t>(on & 0xFF),
      static_cast<uint8_t>(on >> 8),
      static_cast<uint8_t>(off & 0xFF),
      static_cast<uint8_t>(off >> 8),
  };
  return i2c_master_transmit(dev_, payload, sizeof(payload), 50);
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
  // Datasheet § 7.3.1.1: after clearing SLEEP, the internal oscillator
  // takes up to 500 µs to stabilise. Writing the RESTART bit (or any
  // PWM channel) before that races the prescaler latch and the first
  // few PWM cycles come out at the wrong frequency. 1 ms is the
  // smallest tick at FreeRTOS_HZ=1000 and comfortably covers it.
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
  return WriteLedQuad(kRegLed0OnL + 4 * channel, duty);
}

esp_err_t Pca9685::SetAll(uint16_t duty) {
  // Single 5-byte broadcast via ALL_LED_* registers — saves 15 I2C
  // transactions vs looping SetDuty per channel.
  return WriteLedQuad(kRegAllLedOnL, duty);
}

}  // namespace btclock
