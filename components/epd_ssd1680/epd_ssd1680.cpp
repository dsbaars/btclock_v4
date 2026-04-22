#include "epd_ssd1680.hpp"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "ssd1680";

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
constexpr uint8_t kCmdSetLut = 0x32;

// GxEPD2_213_BN::lut_partial — 153 bytes.
constexpr uint8_t kPartialLut[153] = {
    0x0, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0A, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2,
    0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
};
static_assert(sizeof(kPartialLut) == 153, "SSD1680 LUT is 153 bytes");

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

// -------- EpdIoPin --------

const char* EpdIoPin::Describe() const {
  // Rotating small buffer pool so multiple Describe() calls in the same
  // printf statement don't clobber one another. Not thread-safe but the
  // only caller is the ESP_LOGI lines inside EpdPanel which run on one
  // task at boot.
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
    // SetDirection (pin-level) — kOutput. Then seed the output latch.
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

// -------- EpdBus --------

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
  if (err != ESP_OK) { cs.Write(true); return err; }
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

// -------- EpdPanel --------

EpdPanel::EpdPanel(const Config& cfg) : cfg_(cfg) {
  ESP_ERROR_CHECK(cfg_.cs.ConfigureAsOutput(true));
  ESP_ERROR_CHECK(cfg_.busy.ConfigureAsInput());
  // RESET direction is asserted explicitly in HardReset() — its MCP
  // siblings on the same port may not have had their direction set yet
  // when the first panel is constructed. The Mcp23017 driver in this
  // tree treats SetDirection as idempotent.
}

EpdPanel::~EpdPanel() {
  if (shadow_ != nullptr) heap_caps_free(shadow_);
}

int EpdPanel::Width() const {
  switch (cfg_.kind) {
    case PanelKind::k2_13: return 122;
    case PanelKind::k2_9:  return 128;
  }
  return 0;
}

int EpdPanel::Height() const {
  switch (cfg_.kind) {
    case PanelKind::k2_13: return 250;
    case PanelKind::k2_9:  return 296;
  }
  return 0;
}

void EpdPanel::HardReset() {
  // RESET_n is active-low on SSD1680. Direction is re-asserted on each
  // reset because V8 constructs all panels before any are initialised,
  // so the MCP port direction register may still be bit-shuffled.
  cfg_.reset.ConfigureAsOutput(true);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  cfg_.reset.Write(true);
  vTaskDelay(pdMS_TO_TICKS(20));
}

// When BUSY is MCP-backed (V8), each poll is an I2C read (~1 ms). Poll
// 10× slower so we don't clobber the bus during a 2+ second full refresh.
uint32_t EpdPanel::BusyPollMs() const {
  return cfg_.busy.is_mcp() ? 20 : 2;
}

void EpdPanel::WaitIdle(uint32_t timeout_ms) {
  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
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

bool EpdPanel::IsIdle() const { return !cfg_.busy.Read(); }

esp_err_t EpdPanel::WaitForRefresh(uint32_t timeout_ms) {
  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(BusyPollMs());
  while (cfg_.busy.Read()) {
    if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(step);
  }
  return ESP_OK;
}

esp_err_t EpdPanel::RewindRam() {
  const uint8_t xcnt = 0x00;
  ESP_RETURN_ON_ERROR(
      cfg_.bus->SendCommand(cfg_.cs, kCmdRamXCounter, &xcnt, 1), kTag,
      "xcnt");
  const uint8_t ycnt[2] = {0x00, 0x00};
  ESP_RETURN_ON_ERROR(
      cfg_.bus->SendCommand(cfg_.cs, kCmdRamYCounter, ycnt, 2), kTag,
      "ycnt");
  return ESP_OK;
}

esp_err_t EpdPanel::WriteVram(uint8_t write_cmd, const uint8_t* fb) {
  ESP_RETURN_ON_ERROR(RewindRam(), kTag, "rewind");
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, write_cmd), kTag,
                      "vram.cmd");
  ESP_RETURN_ON_ERROR(
      cfg_.bus->SendData(cfg_.cs, fb, static_cast<size_t>(FrameBytes())),
      kTag, "vram.data");
  return ESP_OK;
}

esp_err_t EpdPanel::WriteInitCommands() {
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;

  const int h = Height();
  const uint8_t dout[3] = {static_cast<uint8_t>((h - 1) & 0xFF),
                           static_cast<uint8_t>((h - 1) >> 8), 0x00};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDriverOutputCtl, dout, 3),
                      kTag, "dout");
  const uint8_t dem = 0x03;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataEntryMode, &dem, 1), kTag,
                      "dem");
  const uint8_t ramx[2] = {0x00, 0x0F};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamX, ramx, 2), kTag, "ramx");
  const uint8_t ramy[4] = {0x00, 0x00,
                           static_cast<uint8_t>((h - 1) & 0xFF),
                           static_cast<uint8_t>((h - 1) >> 8)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamY, ramy, 4), kTag, "ramy");
  const uint8_t border = 0x05;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdBorderWaveform, &border, 1),
                      kTag, "border");
  const uint8_t duc1[2] = {0x00, 0x80};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl1, duc1, 2), kTag,
                      "duc1");
  const uint8_t temp = 0x80;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdTempSensor, &temp, 1), kTag,
                      "temp");
  const uint8_t xcnt = 0x00;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamXCounter, &xcnt, 1), kTag,
                      "xcnt");
  const uint8_t ycnt[2] = {0x00, 0x00};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamYCounter, ycnt, 2), kTag,
                      "ycnt");
  WaitIdle(5000);
  return ESP_OK;
}

esp_err_t EpdPanel::LoadPartialLut() {
  // Simple LUT load — no hard reset, no warm-up activation. The shadow-
  // based red-VRAM seeding below gives the controller a correct baseline
  // on every partial, so the fancy Waveshare init_part dance isn't needed.
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, kCmdSetLut,
                                             kPartialLut, sizeof(kPartialLut)),
                      kTag, "lut");
  partial_lut_loaded_ = true;
  return ESP_OK;
}

esp_err_t EpdPanel::Init() {
  HardReset();
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, kCmdSwReset), kTag,
                      "swreset");
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(WriteInitCommands(), kTag, "init cmds");
  partial_lut_loaded_ = false;

  if (shadow_ == nullptr) {
    shadow_ = static_cast<uint8_t*>(
        heap_caps_malloc(FrameBytes(), MALLOC_CAP_SPIRAM));
    if (shadow_ == nullptr) {
      ESP_LOGE(kTag, "shadow alloc failed for panel cs=%s", cfg_.cs.Describe());
      return ESP_ERR_NO_MEM;
    }
    std::memset(shadow_, 0xFF, FrameBytes());   // white = cleared
  }

  ESP_LOGI(kTag, "panel init ok kind=%s %dx%d cs=%s busy=%s reset=%s",
           cfg_.kind == PanelKind::k2_13 ? "2.13" : "2.9",
           Width(), Height(),
           cfg_.cs.Describe(), cfg_.busy.Describe(), cfg_.reset.Describe());
  return ESP_OK;
}

esp_err_t EpdPanel::DrawFramebufferStart(const uint8_t* fb, RefreshKind kind) {
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;

  if (kind == RefreshKind::kPartial) {
    if (!partial_lut_loaded_) {
      ESP_RETURN_ON_ERROR(LoadPartialLut(), kTag, "load partial lut");
    }
    // Red VRAM = shadow = the frame currently displayed (the "previous"
    // input for the controller's diff). Black VRAM = new frame.
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, shadow_), kTag,
                        "partial.red");
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "partial.bw");
    const uint8_t duc2 = 0xCC;
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2, &duc2, 1),
                        kTag, "duc2 partial");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate partial");
  } else {
    if (partial_lut_loaded_) {
      // Flush the custom LUT and the partial-mode state by re-initing.
      HardReset();
      WaitIdle(5000);
      ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSwReset), kTag,
                          "swreset.refull");
      WaitIdle(5000);
      ESP_RETURN_ON_ERROR(WriteInitCommands(), kTag, "init.refull");
      partial_lut_loaded_ = false;
    }
    // Full refresh: write the new frame to both buffers so red is
    // primed for the next partial.
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "full.bw");
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, fb), kTag, "full.red");
    const uint8_t duc2 = 0xF7;
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2, &duc2, 1),
                        kTag, "duc2 full");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate full");
  }
  // Update the shadow — caller's `fb` is now what the panel is about
  // to display.
  std::memcpy(shadow_, fb, FrameBytes());
  return ESP_OK;
}

esp_err_t EpdPanel::DrawFramebuffer(const uint8_t* fb, RefreshKind kind,
                                    uint32_t timeout_ms) {
  ESP_RETURN_ON_ERROR(DrawFramebufferStart(fb, kind), kTag, "start");
  return WaitForRefresh(timeout_ms);
}

}  // namespace btclock
