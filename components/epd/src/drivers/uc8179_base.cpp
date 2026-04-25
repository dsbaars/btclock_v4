// UC8179 base — UNTESTED. See uc8179_base.hpp for the caveat block.
//
// This implements the GxEPD2_750_GDEY075T7 init / refresh sequence
// register-for-register. None of it has been driven against real
// silicon in this firmware. The scaffold is here so a follow-up
// bring-up has somewhere to land its tweaks rather than starting
// from scratch.

#include "epd/drivers/uc8179_base.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace epd {

namespace {
constexpr const char* kTag = "uc8179";

constexpr uint8_t kCmdPanelSetting = 0x00;
constexpr uint8_t kCmdPowerSetting = 0x01;
constexpr uint8_t kCmdPowerOff = 0x02;
constexpr uint8_t kCmdPowerOn = 0x04;
constexpr uint8_t kCmdBoosterSoftStart = 0x06;
constexpr uint8_t kCmdDeepSleep = 0x07;
constexpr uint8_t kCmdDataStartTrans1 = 0x10;  // previous frame
constexpr uint8_t kCmdDisplayRefresh = 0x12;
constexpr uint8_t kCmdDataStartTrans2 = 0x13;  // current frame
constexpr uint8_t kCmdDualSpi = 0x15;
constexpr uint8_t kCmdVcomDataInterval = 0x50;
constexpr uint8_t kCmdTconSetting = 0x60;
constexpr uint8_t kCmdResolution = 0x61;
constexpr uint8_t kCmdPowerSaving = 0xE3;
constexpr uint8_t kCmdCascadeSetting = 0xE0;
constexpr uint8_t kCmdForceTemp = 0xE5;
}  // namespace

Uc8179Base::Uc8179Base(const PanelConfig& cfg) : cfg_(cfg) {
  ESP_ERROR_CHECK(cfg_.cs.ConfigureAsOutput(true));
  ESP_ERROR_CHECK(cfg_.busy.ConfigureAsInput());
}

Uc8179Base::~Uc8179Base() = default;

void Uc8179Base::HardReset() {
  cfg_.reset.ConfigureAsOutput(true);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(true);
  vTaskDelay(pdMS_TO_TICKS(20));
  power_is_on_ = false;
}

uint32_t Uc8179Base::BusyPollMs() const {
  return cfg_.busy.is_mcp() ? 20 : 5;
}

void Uc8179Base::WaitIdle(uint32_t timeout_ms) {
  // UC8179 BUSY is active LOW during refresh — the chip pulls the
  // line low while busy and releases (HIGH) when idle. This is the
  // opposite of SSD1680. Mirror GxEPD2's _waitWhileBusy(false) for
  // GDEY075T7: wait for HIGH.
  vTaskDelay(pdMS_TO_TICKS(1));
  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(BusyPollMs());
  while (!cfg_.busy.Read()) {
    if (xTaskGetTickCount() >= deadline) {
      ESP_LOGW(kTag, "cs=%s busy stuck after %ums", cfg_.cs.Describe(),
               static_cast<unsigned>(timeout_ms));
      return;
    }
    vTaskDelay(step);
  }
}

bool Uc8179Base::IsIdle() const { return cfg_.busy.Read(); }

esp_err_t Uc8179Base::PowerOn() {
  if (power_is_on_) return ESP_OK;
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, kCmdPowerOn), kTag,
                      "poweron");
  // GxEPD2 power_on_time = 140 ms; double for safety.
  WaitIdle(500);
  power_is_on_ = true;
  return ESP_OK;
}

esp_err_t Uc8179Base::PowerOff() {
  if (!power_is_on_) return ESP_OK;
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, kCmdPowerOff), kTag,
                      "poweroff");
  // GxEPD2 power_off_time = 42 ms; round up.
  WaitIdle(200);
  power_is_on_ = false;
  return ESP_OK;
}

esp_err_t Uc8179Base::Init() {
  // Mirrors GxEPD2_750_GDEY075T7::_InitDisplay register-for-register.
  HardReset();
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  // Panel setting — KW: 0x3F, KWR: 0x2F, BWROTP: 0x0F, BWOTP: 0x1F.
  // GxEPD2 uses 0x1F (BWOTP).
  const uint8_t panel = 0x1F;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdPanelSetting, &panel, 1),
                      kTag, "panel");
  // Power setting — same shape as the GxEPD2 reference (internal,
  // VGH/VGL = ±20 V, VDH = 15 V, VDL = -15 V, VDHR = 4.2 V).
  const uint8_t power[5] = {0x07, 0x07, 0x3F, 0x3F, 0x09};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdPowerSetting, power, 5),
                      kTag, "power");
  // Booster soft-start — enhanced display drive (0x06 command).
  const uint8_t booster[4] = {0x17, 0x17, 0x28, 0x17};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdBoosterSoftStart, booster, 4),
                      kTag, "booster");
  // Resolution: source = WIDTH, gate = HEIGHT. Pack big-endian per IC.
  const int w = Width();
  const int h = Height();
  const uint8_t res[4] = {static_cast<uint8_t>(w >> 8),
                          static_cast<uint8_t>(w & 0xFF),
                          static_cast<uint8_t>(h >> 8),
                          static_cast<uint8_t>(h & 0xFF)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdResolution, res, 4), kTag,
                      "res");
  // Dual-SPI disabled.
  const uint8_t dspi = 0x00;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDualSpi, &dspi, 1), kTag,
                      "dspi");
  // VCOM and data interval.
  const uint8_t vcom[2] = {0x29, 0x07};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdVcomDataInterval, vcom, 2),
                      kTag, "vcom");
  // TCON — S2G G2S, 12 (default).
  const uint8_t tcon = 0x22;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdTconSetting, &tcon, 1), kTag,
                      "tcon");
  // Power saving — VCOM 2-line period, source 2 * 660 ns.
  const uint8_t pws = 0x22;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdPowerSaving, &pws, 1), kTag,
                      "pws");
  ESP_LOGI(kTag, "panel init ok %dx%d cs=%s busy=%s reset=%s", w, h,
           cs.Describe(), cfg_.busy.Describe(), cfg_.reset.Describe());
  return ESP_OK;
}

esp_err_t Uc8179Base::DrawFramebufferStart(const uint8_t* fb,
                                            RefreshKind kind) {
  // UC8179 refresh: PowerOn, write current (0x13), refresh (0x12).
  // For full refresh GxEPD2 also writes the previous-image RAM
  // (0x10) so the LUT diffs against a known baseline; partial skips
  // it. Inverted-color is handled the same way as the SSD1680 path
  // — XOR every byte before SPI DMA. Stride is panel-specific (the
  // GDEY075T7 packs 100 bytes per scan-line).
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  ESP_RETURN_ON_ERROR(PowerOn(), kTag, "poweron");
  const size_t n = static_cast<size_t>(FrameBytes());
  if (kind == RefreshKind::kFull) {
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataStartTrans1), kTag,
                        "prev.cmd");
    ESP_RETURN_ON_ERROR(bus->SendData(cs, fb, n), kTag, "prev.data");
  }
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataStartTrans2), kTag,
                      "cur.cmd");
  ESP_RETURN_ON_ERROR(bus->SendData(cs, fb, n), kTag, "cur.data");
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDisplayRefresh), kTag,
                      "refresh");
  return ESP_OK;
}

esp_err_t Uc8179Base::WaitForRefresh(uint32_t timeout_ms) {
  WaitIdle(timeout_ms);
  return ESP_OK;
}

}  // namespace epd
}  // namespace btclock
