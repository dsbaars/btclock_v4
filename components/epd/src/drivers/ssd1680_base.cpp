#include "epd/drivers/ssd1680_base.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace epd {

namespace {
constexpr const char* kTag = "ssd1680";

// Command map follows the SSD1680 datasheet and the GDEY0213B74 /
// GDEY029T94 panel datasheets.
constexpr uint8_t kCmdSwReset = 0x12;
constexpr uint8_t kCmdDriverOutputCtl = 0x01;
constexpr uint8_t kCmdDataEntryMode = 0x11;
constexpr uint8_t kCmdSetRamX = 0x44;
constexpr uint8_t kCmdSetRamY = 0x45;
constexpr uint8_t kCmdBorderWaveform = 0x3C;
constexpr uint8_t kCmdDispUpdateCtl1 = 0x21;
constexpr uint8_t kCmdTempSensor = 0x18;
constexpr uint8_t kCmdRamXCounter = 0x4E;
constexpr uint8_t kCmdRamYCounter = 0x4F;
constexpr uint8_t kCmdWriteBwRam = 0x24;
constexpr uint8_t kCmdWriteRedRam = 0x26;
constexpr uint8_t kCmdDispUpdateCtl2 = 0x22;
constexpr uint8_t kCmdActivateUpdate = 0x20;
constexpr uint8_t kCmdTempReg = 0x1A;  // fast-full path only

}  // namespace

Ssd1680Base::Ssd1680Base(const PanelConfig& cfg) : cfg_(cfg) {
  ESP_ERROR_CHECK(cfg_.cs.ConfigureAsOutput(true));
  ESP_ERROR_CHECK(cfg_.busy.ConfigureAsInput());
  // RESET direction is asserted explicitly inside HardReset() — its
  // MCP siblings on the same port may not have had their direction
  // set yet when the first panel is constructed.
}

Ssd1680Base::~Ssd1680Base() {
  if (shadow_ != nullptr) heap_caps_free(shadow_);
  if (invert_scratch_ != nullptr) heap_caps_free(invert_scratch_);
}

void Ssd1680Base::HardReset() {
  cfg_.reset.ConfigureAsOutput(true);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(true);
  vTaskDelay(pdMS_TO_TICKS(20));
}

uint32_t Ssd1680Base::BusyPollMs() const {
  // V8 routes BUSY through MCP — each poll is an I2C read (~1 ms).
  // Slow down 10x there; otherwise we'd clobber the bus during a
  // ~2 s full refresh.
  return cfg_.busy.is_mcp() ? 20 : 2;
}

void Ssd1680Base::WaitIdle(uint32_t timeout_ms) {
  // SSD1680 BUSY goes HIGH within ~1 ms of an activate; if we read
  // immediately we could see "idle" before the chip even starts.
  // Use a delay(1) prelude to ensure the chip has started the refresh.
  vTaskDelay(pdMS_TO_TICKS(1));
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(BusyPollMs());
  while (cfg_.busy.Read()) {
    if (xTaskGetTickCount() >= deadline) {
      ESP_LOGW(kTag, "cs=%s busy stuck after %ums", cfg_.cs.Describe(),
               static_cast<unsigned>(timeout_ms));
      return;
    }
    vTaskDelay(step);
  }
}

bool Ssd1680Base::IsIdle() const {
  return !cfg_.busy.Read();
}

esp_err_t Ssd1680Base::WaitForRefresh(uint32_t timeout_ms) {
  vTaskDelay(pdMS_TO_TICKS(1));
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(BusyPollMs());
  while (cfg_.busy.Read()) {
    if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(step);
  }
  // Conditional power-off based on the last refresh kind. The 0xF7
  // full waveform implicitly disables the analog block as part of
  // the OTP sequence; sending
  // 0x83 + activate to an already-off chip wastes the 500 ms BUSY-
  // stuck timeout per panel because the chip can't drive BUSY high
  // without analog. So power off only after a partial refresh.
  //
  // SetGlobalFastPartial(true) also skips the power-off cycle: the
  // very next partial's DUC2=0xFC re-enables the analog anyway, so
  // toggling it off+on between back-to-back partials is pure waste
  // (~10-30 ms/frame). kFull-after-fast-partial recovers because
  // DrawFramebufferStart's kFull path always HardReset+ReInits.
  if (last_kind_ == RefreshKind::kPartial && !GetGlobalFastPartial()) {
    const uint8_t duc2_off = 0x83;
    esp_err_t off =
        cfg_.bus->SendCommand(cfg_.cs, kCmdDispUpdateCtl2, &duc2_off, 1);
    if (off == ESP_OK) off = cfg_.bus->SendCommand(cfg_.cs, kCmdActivateUpdate);
    if (off == ESP_OK) {
      const TickType_t off_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
      while (cfg_.busy.Read()) {
        if (xTaskGetTickCount() >= off_deadline) {
          ESP_LOGW(kTag, "cs=%s power-off busy stuck", cfg_.cs.Describe());
          break;
        }
        vTaskDelay(step);
      }
    } else {
      ESP_LOGW(kTag, "cs=%s power-off send failed %d", cfg_.cs.Describe(), off);
    }
  }
  return ESP_OK;
}

esp_err_t Ssd1680Base::WriteDriverOutputControl() {
  // GDEY0213B74 datasheet specifies 0xF9, 0x00, 0x00 (HEIGHT-1 = 250-1).
  // Emit the same shape parametrised on Height() so the 2.9" override
  // (0x0127 LE) shares the same code path. GDEY029T94 explicitly
  // overrides this method for safety.
  const int h = Height();
  const uint8_t dout[3] = {static_cast<uint8_t>((h - 1) & 0xFF),
                           static_cast<uint8_t>((h - 1) >> 8), 0x00};
  return cfg_.bus->SendCommand(cfg_.cs, kCmdDriverOutputCtl, dout, 3);
}

esp_err_t Ssd1680Base::SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w,
                                         uint16_t h) {
  // Configures the partial-RAM area — entry mode + RAM bounds +
  // counters in one go. Re-applied per-frame from DrawFramebufferStart
  // because a SPI transaction interrupted mid-byte by a high-priority
  // task can leave the controller in an unexpected state; reinforce
  // on every refresh as a defensive measure.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  const uint8_t dem = 0x03;  // x increase, y increase
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataEntryMode, &dem, 1), kTag,
                      "dem");
  const uint8_t ramx[2] = {static_cast<uint8_t>(x / 8),
                           static_cast<uint8_t>((x + w - 1) / 8)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamX, ramx, 2), kTag, "ramx");
  const uint8_t ramy[4] = {
      static_cast<uint8_t>(y % 256),
      static_cast<uint8_t>(y / 256),
      static_cast<uint8_t>((y + h - 1) % 256),
      static_cast<uint8_t>((y + h - 1) / 256),
  };
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamY, ramy, 4), kTag, "ramy");
  const uint8_t xcnt = static_cast<uint8_t>(x / 8);
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamXCounter, &xcnt, 1), kTag,
                      "xcnt");
  const uint8_t ycnt[2] = {static_cast<uint8_t>(y % 256),
                           static_cast<uint8_t>(y / 256)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamYCounter, ycnt, 2), kTag,
                      "ycnt");
  return ESP_OK;
}

esp_err_t Ssd1680Base::WriteVram(uint8_t write_cmd, const uint8_t* fb) {
  ESP_RETURN_ON_ERROR(SetPartialRamArea(0, 0, Width(), Height()), kTag,
                      "rewind");
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, write_cmd), kTag,
                      "vram.cmd");
  const size_t n = static_cast<size_t>(FrameBytes());
  const uint8_t* to_send = fb;
  if (GetGlobalInverted()) {
    if (invert_scratch_ == nullptr) {
      invert_scratch_ =
          static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
      if (invert_scratch_ == nullptr) {
        ESP_LOGE(kTag, "invert scratch alloc failed for panel cs=%s",
                 cfg_.cs.Describe());
        return ESP_ERR_NO_MEM;
      }
    }
    for (size_t i = 0; i < n; ++i) invert_scratch_[i] = fb[i] ^ 0xFFu;
    to_send = invert_scratch_;
  }
  ESP_RETURN_ON_ERROR(cfg_.bus->SendData(cfg_.cs, to_send, n), kTag,
                      "vram.data");
  return ESP_OK;
}

esp_err_t Ssd1680Base::WriteInitCommands() {
  // _InitDisplay equivalent. Driver output control + data-entry mode
  // (folded into SetPartialRamArea) + border + DUC1 + temp sensor.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  ESP_RETURN_ON_ERROR(WriteDriverOutputControl(), kTag, "dout");
  const uint8_t border = BorderWaveform();
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdBorderWaveform, &border, 1),
                      kTag, "border");
  uint8_t duc1[2];
  DispUpdateControl1(duc1);
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl1, duc1, 2), kTag,
                      "duc1");
  const uint8_t temp = 0x80;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdTempSensor, &temp, 1), kTag,
                      "temp");
  ESP_RETURN_ON_ERROR(SetPartialRamArea(0, 0, Width(), Height()), kTag,
                      "init.ram");
  WaitIdle(5000);
  return ESP_OK;
}

esp_err_t Ssd1680Base::Init() {
  HardReset();
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, kCmdSwReset), kTag,
                      "swreset");
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(WriteInitCommands(), kTag, "init cmds");

  if (shadow_ == nullptr) {
    shadow_ = static_cast<uint8_t*>(
        heap_caps_malloc(FrameBytes(), MALLOC_CAP_SPIRAM));
    if (shadow_ == nullptr) {
      ESP_LOGE(kTag, "shadow alloc failed for panel cs=%s", cfg_.cs.Describe());
      return ESP_ERR_NO_MEM;
    }
    std::memset(shadow_, 0xFF, FrameBytes());  // white = cleared
  }

  ESP_LOGI(kTag, "panel init ok %dx%d cs=%s busy=%s reset=%s", Width(),
           Height(), cfg_.cs.Describe(), cfg_.busy.Describe(),
           cfg_.reset.Describe());
  return ESP_OK;
}

esp_err_t Ssd1680Base::DrawFramebufferStart(const uint8_t* fb,
                                            RefreshKind kind) {
  // SSD1680 partial / full refresh sequences per the datasheet.
  //   * NO custom LUT upload — OTP waveforms keyed by DUC2 do the work.
  //   * Partial writes only 0x24 (BW); for the 2.13" we additionally
  //     prime 0x26 with the previous frame, which the chip otherwise
  //     fails to copy automatically.
  //   * Full writes both 0x24 and 0x26 so the OTP full waveform sees
  //     a consistent previous == current baseline.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  last_kind_ = kind;

  // Per-refresh re-init mirrors v3_fci's display.init(0, false, 40)
  // before every refresh — leaves controller in a known state and
  // makes the shadow→0x26 priming below take cleanly on subsequent
  // partials. SSD1680 RAM survives HW reset (only registers clear).
  // SetGlobalFastPartial(true) lets a continuous-partial workload
  // (the boot spinner) skip this ~80 ms block; kFull still re-inits
  // so the next data render recovers a known state regardless.
  if (kind != RefreshKind::kPartial || !GetGlobalFastPartial()) {
    HardReset();
    WaitIdle(5000);
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSwReset), kTag, "swreset");
    WaitIdle(5000);
    ESP_RETURN_ON_ERROR(WriteInitCommands(), kTag, "init cmds");
  }

  if (kind == RefreshKind::kPartial) {
    if (PrimePartialPreviousRam()) {
      ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, shadow_), kTag,
                          "partial.red");
    }
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "partial.bw");
    const uint8_t duc2 = Duc2Partial();
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2, &duc2, 1),
                        kTag, "duc2 partial");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate partial");
  } else {
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, fb), kTag, "full.red");
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "full.bw");
    if (UseFastFullUpdate()) {
      const uint8_t temp_override = 0x64;
      ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdTempReg, &temp_override, 1),
                          kTag, "temp override");
    }
    const uint8_t duc2 = Duc2Full();
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2, &duc2, 1),
                        kTag, "duc2 full");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate full");
  }
  std::memcpy(shadow_, fb, FrameBytes());
  return ESP_OK;
}

}  // namespace epd
}  // namespace btclock
