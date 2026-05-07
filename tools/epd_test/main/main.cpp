// Minimal EPD driver sanity test for Rev B. No wifi, no data sources,
// no screen manager — just hardware bring-up, then an endless loop
// painting a seconds counter across the 7 panels:
//   panels 0..2 — normal polarity (white background, black digits)
//   panels 3..5 — inverted polarity (black background, white digits)
//   panel  6    — solid fill that flips every second
// First frame uses full refresh; every subsequent frame uses partial.
// Photograph the panel to see whether partial-refresh artefacts reported
// by the user reproduce without any of the app plumbing.

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include "driver/gpio.h"
#include "epd/factory.hpp"
#include "epd/panel.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.hpp"
#include "mcp23017.hpp"
#include "pca9685.hpp"

namespace {

constexpr const char* kTag = "epd_test";

// Rev B pinout — copied from main/board/board_rev_b.hpp so this test
// project is self-contained.
constexpr gpio_num_t kI2cSda = GPIO_NUM_35;
constexpr gpio_num_t kI2cScl = GPIO_NUM_36;
constexpr uint16_t kMcp1Addr = 0x20;
constexpr uint16_t kPcaAddr = 0x42;
constexpr gpio_num_t kPcaOe = GPIO_NUM_48;
constexpr gpio_num_t kEpdSpiSclk = GPIO_NUM_12;
constexpr gpio_num_t kEpdSpiMosi = GPIO_NUM_11;
constexpr gpio_num_t kEpdDc = GPIO_NUM_14;

constexpr int kNumPanels = 7;
constexpr gpio_num_t kEpdCs[kNumPanels] = {
    GPIO_NUM_2,  GPIO_NUM_4,  GPIO_NUM_6,  GPIO_NUM_10,
    GPIO_NUM_38, GPIO_NUM_21, GPIO_NUM_17,
};
constexpr gpio_num_t kEpdBusy[kNumPanels] = {
    GPIO_NUM_3,  GPIO_NUM_5,  GPIO_NUM_7,  GPIO_NUM_9,
    GPIO_NUM_37, GPIO_NUM_18, GPIO_NUM_16,
};
constexpr uint8_t kEpdResetMcp[kNumPanels] = {8, 9, 10, 11, 12, 13, 14};

// 2.13" panel dimensions. Stride is fixed at 16 in the driver.
constexpr int kPanelW = 122;
constexpr int kPanelH = 250;
constexpr int kStride = 16;
constexpr int kFrameBytes = kStride * kPanelH;

// Framebuffer convention (matches driver): bit=1 means white pixel,
// bit=0 means black. MSB is the leftmost pixel in each stride byte.

// Compact 5x7 bitmap font for digits 0-9. Each digit is 7 rows, each
// row is one byte with the glyph in the top 5 bits (MSB-aligned).
constexpr uint8_t kDigit5x7[10][7] = {
    {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70},  // 0
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70},  // 1
    {0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8},  // 2
    {0xF0, 0x08, 0x08, 0x70, 0x08, 0x08, 0xF0},  // 3
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10},  // 4
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70},  // 5
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70},  // 6
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40},  // 7
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70},  // 8
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60},  // 9
};

inline void SetPx(uint8_t* fb, int x, int y, bool white) {
  if (x < 0 || x >= kPanelW || y < 0 || y >= kPanelH) return;
  const int idx = y * kStride + (x >> 3);
  const uint8_t mask = 0x80 >> (x & 7);
  if (white) {
    fb[idx] |= mask;
  } else {
    fb[idx] &= ~mask;
  }
}

void FillRect(uint8_t* fb, int x, int y, int w, int h, bool white) {
  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      SetPx(fb, x + dx, y + dy, white);
    }
  }
}

void DrawDigit(uint8_t* fb, int x0, int y0, int scale, int digit,
               bool fg_white) {
  const uint8_t* rows = kDigit5x7[digit % 10];
  for (int r = 0; r < 7; ++r) {
    const uint8_t row = rows[r];
    for (int c = 0; c < 5; ++c) {
      if (row & (0x80 >> c)) {
        FillRect(fb, x0 + c * scale, y0 + r * scale, scale, scale, fg_white);
      }
    }
  }
}

// Two-digit centered "NN" counter. Scale 10 → digit is 50×70.
void RenderTwoDigits(uint8_t* fb, int value, bool bg_white) {
  std::memset(fb, bg_white ? 0xFF : 0x00, kFrameBytes);
  const int scale = 10;
  const int digit_w = 5 * scale;
  const int digit_h = 7 * scale;
  const int gap = 12;
  const int total_w = digit_w * 2 + gap;
  const int x0 = (kPanelW - total_w) / 2;
  const int y0 = (kPanelH - digit_h) / 2;
  const int tens = (value / 10) % 10;
  const int ones = value % 10;
  const bool fg_white = !bg_white;
  DrawDigit(fb, x0, y0, scale, tens, fg_white);
  DrawDigit(fb, x0 + digit_w + gap, y0, scale, ones, fg_white);
}

}  // namespace

extern "C" void app_main() {
  // Intentionally no nvs_flash_init — this test doesn't persist
  // anything, and skipping it means OTA-uploading the binary onto a
  // device running the real firmware won't risk wiping its NVS (the
  // "new version / no free pages" branches would call nvs_flash_erase).
  ESP_LOGI(kTag, "i2c up");
  btclock::I2cBus i2c(I2C_NUM_0, kI2cSda, kI2cScl);

  ESP_LOGI(kTag, "mcp up");
  btclock::Mcp23017 mcp(i2c, kMcp1Addr);
  ESP_ERROR_CHECK(mcp.SetDirectionPort(0xFFFF));
  // Rev B: RESET on pins 8..14, set those as outputs preloaded HIGH so
  // the panels come out of reset clean.
  for (int p = 8; p <= 14; ++p) {
    ESP_ERROR_CHECK(mcp.SetDirection(p, btclock::Mcp23017::PinMode::kOutput));
  }
  ESP_ERROR_CHECK(mcp.WritePort(0xFF00));

  ESP_LOGI(kTag, "pca up → frontlight 50%%");
  btclock::Pca9685 pca(i2c, kPcaAddr, kPcaOe);
  ESP_ERROR_CHECK(pca.Begin(1000));
  pca.SetOutputEnable(true);
  for (uint8_t ch = 1; ch <= 7; ++ch) {
    ESP_ERROR_CHECK(pca.SetDuty(ch, 2048));
  }

  ESP_LOGI(kTag, "spi + panels up");
  btclock::EpdBus bus(SPI2_HOST, kEpdSpiSclk, kEpdSpiMosi, kEpdDc,
                      4 * 1000 * 1000, 16 * 296 + 64);
  std::array<std::unique_ptr<btclock::epd::IEpdPanel>, kNumPanels> panels;
  for (int i = 0; i < kNumPanels; ++i) {
    btclock::epd::PanelConfig cfg = {};
    cfg.bus = &bus;
    cfg.cs = btclock::EpdIoPin::Native(kEpdCs[i]);
    cfg.busy = btclock::EpdIoPin::Native(kEpdBusy[i]);
    cfg.reset = btclock::EpdIoPin::Mcp(&mcp, kEpdResetMcp[i]);
    panels[i] = btclock::epd::CreatePanel(cfg);
    ESP_ERROR_CHECK(panels[i]->Init());
  }

  // Per-panel framebuffers live on the main task stack (8 KiB in
  // sdkconfig.defaults → plenty of headroom for 7 × 4 kB). If the stack
  // ever shrinks, move these to a heap allocation.
  static uint8_t fb[kNumPanels][kFrameBytes];

  ESP_LOGI(kTag, "entering render loop");
  int seconds = 0;
  while (true) {
    for (int i = 0; i < kNumPanels; ++i) {
      if (i == 6) {
        // Indicator panel — solid white/black alternation every second.
        const bool bg_white = (seconds & 1);
        std::memset(fb[i], bg_white ? 0xFF : 0x00, kFrameBytes);
      } else {
        // 0..2 normal (white bg), 3..5 inverted (black bg).
        const bool bg_white = (i < 3);
        RenderTwoDigits(fb[i], seconds % 100, bg_white);
      }
    }

    const btclock::RefreshKind kind = (seconds == 0)
                                          ? btclock::RefreshKind::kFull
                                          : btclock::RefreshKind::kPartial;
    const int64_t t_start = esp_timer_get_time();
    for (int i = 0; i < kNumPanels; ++i) {
      ESP_ERROR_CHECK(panels[i]->DrawFramebufferStart(fb[i], kind));
    }
    for (int i = 0; i < kNumPanels; ++i) {
      const esp_err_t rc = panels[i]->WaitForRefresh(5000);
      if (rc != ESP_OK) {
        ESP_LOGW(kTag, "panel %d refresh timeout", i);
      }
    }
    const int64_t ms = (esp_timer_get_time() - t_start) / 1000;
    ESP_LOGI(kTag, "s=%d kind=%s took=%lldms", seconds,
             kind == btclock::RefreshKind::kFull ? "FULL" : "PART", ms);

    ++seconds;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
