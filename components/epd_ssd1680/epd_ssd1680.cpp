#include "epd_ssd1680.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {

// Global polarity flag — when true, WriteVram inverts each byte with
// XOR 0xFF before the SPI DMA. One flag is enough: the panels all share
// the same framebuffer-bit convention (1 = white, 0 = black) so flipping
// at the flush layer gives every renderer the inverted look without any
// per-renderer change. std::atomic so a PATCH-driven toggle on the
// webserver task is visible to the main task's next render.
static std::atomic<bool> g_inverted{false};

void EpdSetGlobalInverted(bool inverted) { g_inverted.store(inverted); }
bool EpdGetGlobalInverted() { return g_inverted.load(); }

namespace {
constexpr const char* kTag = "ssd1680";

// Command map follows the SSD1680 datasheet and GxEPD2's port of the
// GDEY0213B74 reference driver (src/gdey/GxEPD2_213_GDEY0213B74.cpp).
// Exact GxEPD2 lines are quoted at each use site.
constexpr uint8_t kCmdSwReset = 0x12;          // GxEPD2 _InitDisplay:355
constexpr uint8_t kCmdDriverOutputCtl = 0x01;  // _InitDisplay:357
constexpr uint8_t kCmdDataEntryMode = 0x11;    // _InitDisplay:361, _setPartialRamArea:309
constexpr uint8_t kCmdSetRamX = 0x44;          // _setPartialRamArea:311
constexpr uint8_t kCmdSetRamY = 0x45;          // _setPartialRamArea:314
constexpr uint8_t kCmdBorderWaveform = 0x3C;   // _InitDisplay:363
constexpr uint8_t kCmdDispUpdateCtl1 = 0x21;   // _InitDisplay:365
constexpr uint8_t kCmdTempSensor = 0x18;       // _InitDisplay:368
// kCmdTempReg (0x1A) and the fast-full temperature-override byte
// (0x64) lived here to configure the 0xD7 fast-full waveform. The
// slow-full waveform (0xF7) runs the OTP LUT without needing a
// temp-reg write; removed to keep the command map tight. Re-add if
// fast-full is ever re-enabled behind a feature flag.
constexpr uint8_t kCmdRamXCounter = 0x4E;      // _setPartialRamArea:319
constexpr uint8_t kCmdRamYCounter = 0x4F;      // _setPartialRamArea:321
constexpr uint8_t kCmdWriteBwRam = 0x24;       // writeImage:57 / _writeImage:95
constexpr uint8_t kCmdWriteRedRam = 0x26;      // writeImageForFullRefresh:62 / clearScreen:24
constexpr uint8_t kCmdDispUpdateCtl2 = 0x22;   // _Update_Full:380, _Update_Part:395
constexpr uint8_t kCmdActivateUpdate = 0x20;   // _Update_Full:388, _Update_Part:397
// (0x10 deep-sleep / hibernate not ported — v4 keeps panels powered
//  continuously; a 0x10 on top of _PowerOff is available in GxEPD2 at
//  GxEPD2_213_GDEY0213B74.cpp:300 if a future power-save mode is
//  wired up.)

// Display-update-control-2 payloads. 0xf7 is the standard full-update
// waveform (GxEPD2_213_GDEY0213B74.cpp useFastFullUpdate=false path).
// We previously used the fast path 0xd7, but that waveform under-drove
// the white→black transition on invertedColor=true content — panels
// ended up pale-gray instead of true black. The slow waveform runs
// the full LUT sweep and completes in ~1.5 s per panel (vs ~0.8 s
// fast) — acceptable because full refreshes only happen on screen
// change / explicit /api/full_refresh / invertedColor PATCH / boot.
// 0xfc is the OTP partial waveform (GxEPD2 _Update_Part).
constexpr uint8_t kDuc2FullSlow = 0xF7;        // GxEPD2 useFastFullUpdate=false
constexpr uint8_t kDuc2Partial = 0xFC;         // _Update_Part:396


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
  if (invert_scratch_ != nullptr) heap_caps_free(invert_scratch_);
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

// SSD1680 BUSY goes HIGH within ~1 ms of an activate-update (0x20).
// If we start polling BUSY immediately the very first read can come
// back LOW ("idle") because the panel hasn't asserted yet — we'd
// "finish" waiting before the refresh starts, and the caller would
// kick the next SPI command into a mid-refresh controller. GxEPD2's
// _waitWhileBusy handles this with a fixed `delay(1)` before the
// "wait for LOW" loop; mirror that here so we match the reference
// driver's behaviour exactly.
void EpdPanel::WaitIdle(uint32_t timeout_ms) {
  vTaskDelay(pdMS_TO_TICKS(1));
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
  vTaskDelay(pdMS_TO_TICKS(1));
  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(BusyPollMs());
  while (cfg_.busy.Read()) {
    if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(step);
  }
  // Conditional power-off mirroring GxEPD2's state machine
  // (GxEPD2_213_GDEY0213B74.cpp _Update_Full sets _power_is_on=false at
  // line 379, _Update_Part keeps _power_is_on=true at line 388). The
  // 0xF7 full waveform implicitly disables the analog block as part of
  // the OTP sequence; sending 0x83+activate to an already-off chip
  // wastes the 500ms BUSY-stuck timeout per panel because the chip
  // can't drive BUSY high without analog. So power off only after a
  // partial refresh, matching v3_fci epd.cpp:501 (powerOff is a no-op
  // there too when _power_is_on is false after _Update_Full).
  if (last_kind_ == RefreshKind::kPartial) {
    const uint8_t duc2_off = 0x83;
    esp_err_t off = cfg_.bus->SendCommand(cfg_.cs, kCmdDispUpdateCtl2,
                                          &duc2_off, 1);
    if (off == ESP_OK) off = cfg_.bus->SendCommand(cfg_.cs, kCmdActivateUpdate);
    if (off == ESP_OK) {
      const TickType_t off_deadline =
          xTaskGetTickCount() + pdMS_TO_TICKS(500);
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

esp_err_t EpdPanel::RewindRam() {
  // Mirror GxEPD2's _setPartialRamArea (GxEPD2_213_GDEY0213B74.cpp:307-324)
  // — re-establish the data-entry mode AND the RAM x/y bounds before
  // every frame, not just the counters. Earlier we set these once in
  // WriteInitCommands and only rewound counters per frame; that's
  // fragile if the entry-mode register (0x11) or the RAM range
  // registers (0x44 / 0x45) ever drift (e.g. SPI transaction
  // interrupted mid-byte by a high-priority task, leaving the
  // controller in an unexpected state). GxEPD2 plays it safe and
  // reinforces all five every refresh — adopt the same defence.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  const uint8_t dem = 0x03;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataEntryMode, &dem, 1),
                      kTag, "dem");
  // RAM X range: 0x00 .. (Width-1)/8. Width covers the addressable
  // pixel range; for the 2.13" the partition is byte-aligned to 16
  // (visible 122 / addressed 128 pixels), so end = 0x0F.
  const uint8_t ramx[2] = {0x00, static_cast<uint8_t>((Width() - 1) / 8)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamX, ramx, 2), kTag,
                      "ramx");
  const int last_y = Height() - 1;
  const uint8_t ramy[4] = {
      0x00, 0x00,
      static_cast<uint8_t>(last_y & 0xFF),
      static_cast<uint8_t>(last_y >> 8),
  };
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamY, ramy, 4), kTag,
                      "ramy");
  // Counters: x=0, y=0 — start writing at top-left.
  const uint8_t xcnt = 0x00;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamXCounter, &xcnt, 1),
                      kTag, "xcnt");
  const uint8_t ycnt[2] = {0x00, 0x00};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamYCounter, ycnt, 2),
                      kTag, "ycnt");
  return ESP_OK;
}

esp_err_t EpdPanel::WriteVram(uint8_t write_cmd, const uint8_t* fb) {
  ESP_RETURN_ON_ERROR(RewindRam(), kTag, "rewind");
  ESP_RETURN_ON_ERROR(cfg_.bus->SendCommand(cfg_.cs, write_cmd), kTag,
                      "vram.cmd");
  const size_t n = static_cast<size_t>(FrameBytes());
  const uint8_t* to_send = fb;
  if (g_inverted.load()) {
    // Lazy-allocate the per-panel scratch in PSRAM. ~4KB per 2.13"
    // panel, allocated only when the user actually enables the
    // inverted mode so normal installs pay no memory cost.
    if (invert_scratch_ == nullptr) {
      invert_scratch_ = static_cast<uint8_t*>(
          heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
      if (invert_scratch_ == nullptr) {
        ESP_LOGE(kTag, "invert scratch alloc failed for panel cs=%s",
                 cfg_.cs.Describe());
        return ESP_ERR_NO_MEM;
      }
    }
    for (size_t i = 0; i < n; ++i) invert_scratch_[i] = fb[i] ^ 0xFFu;
    to_send = invert_scratch_;
  }
  ESP_RETURN_ON_ERROR(
      cfg_.bus->SendData(cfg_.cs, to_send, n),
      kTag, "vram.data");
  return ESP_OK;
}

esp_err_t EpdPanel::WriteInitCommands() {
  // Mirrors GxEPD2_213_GDEY0213B74::_InitDisplay (src/gdey/
  // GxEPD2_213_GDEY0213B74.cpp:351-372). Called exactly once after a
  // hard reset + SW reset, same sequence as the reference driver —
  // driver output control, data-entry mode, border waveform, DUC1,
  // built-in temperature sensor, then the initial partial-RAM area.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;

  // Driver output control: MUX gate lines = HEIGHT-1, gate_scan_dir =
  // 0x00. GxEPD2 hardcodes 0xF9, 0x00, 0x00 because the reference
  // driver targets only the 250-line GDEY0213B74; v4 supports 2.9"
  // (296 lines) as well, so pass HEIGHT-1 directly.
  //   GxEPD2_213_GDEY0213B74.cpp:357-360.
  const int h = Height();
  const uint8_t dout[3] = {static_cast<uint8_t>((h - 1) & 0xFF),
                           static_cast<uint8_t>((h - 1) >> 8), 0x00};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDriverOutputCtl, dout, 3),
                      kTag, "dout");
  // Data entry mode 0x03 — x-increment, y-increment. GxEPD2 uses 0x01
  // here (y-decrement, x-increment) then overwrites it inside
  // _setPartialRamArea (GxEPD2_213_GDEY0213B74.cpp:309-310) with 0x03
  // before every draw. Skip the redundancy: we always draw full-frame
  // from (0,0) with 0x03, so set it once and leave it.
  const uint8_t dem = 0x03;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDataEntryMode, &dem, 1), kTag,
                      "dem");
  // Partial-RAM area = full screen. Matches _setPartialRamArea(0, 0,
  // WIDTH, HEIGHT) call at GxEPD2_213_GDEY0213B74.cpp:370.
  const uint8_t ramx[2] = {0x00, 0x0F};  // 0x0F * 8 = 120 (2.13" WIDTH=128)
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamX, ramx, 2), kTag, "ramx");
  const uint8_t ramy[4] = {0x00, 0x00,
                           static_cast<uint8_t>((h - 1) & 0xFF),
                           static_cast<uint8_t>((h - 1) >> 8)};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSetRamY, ramy, 4), kTag, "ramy");
  // Border waveform — GxEPD2_213_GDEY0213B74.cpp:363-364.
  const uint8_t border = 0x05;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdBorderWaveform, &border, 1),
                      kTag, "border");
  // Display update control 1: source output = bit7 set → enable RAM
  // content only (no red). GxEPD2_213_GDEY0213B74.cpp:365-367.
  const uint8_t duc1[2] = {0x00, 0x80};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl1, duc1, 2), kTag,
                      "duc1");
  // Built-in temperature sensor — GxEPD2_213_GDEY0213B74.cpp:368-369.
  const uint8_t temp = 0x80;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdTempSensor, &temp, 1), kTag,
                      "temp");
  // Reset RAM counters to origin. GxEPD2 does this inside
  // _setPartialRamArea via 0x4E / 0x4F writes — we fold it into init.
  const uint8_t xcnt = 0x00;
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamXCounter, &xcnt, 1), kTag,
                      "xcnt");
  const uint8_t ycnt[2] = {0x00, 0x00};
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdRamYCounter, ycnt, 2), kTag,
                      "ycnt");
  WaitIdle(5000);
  return ESP_OK;
}

esp_err_t EpdPanel::Init() {
  // Mirrors GxEPD2_EPD::init (GxEPD2_EPD.cpp:44-89) followed by the
  // first call to _InitDisplay triggered from writeScreenBuffer /
  // clearScreen. GxEPD2's init doesn't run SW reset itself — the SW
  // reset lives in _InitDisplay (GxEPD2_213_GDEY0213B74.cpp:355) and
  // fires on the first refresh after init. v4 folds them together so
  // the first DrawFramebufferStart doesn't need to carry the cold-
  // start latency.
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
    std::memset(shadow_, 0xFF, FrameBytes());   // white = cleared
  }

  ESP_LOGI(kTag, "panel init ok kind=%s %dx%d cs=%s busy=%s reset=%s",
           cfg_.kind == PanelKind::k2_13 ? "2.13" : "2.9",
           Width(), Height(),
           cfg_.cs.Describe(), cfg_.busy.Describe(), cfg_.reset.Describe());
  return ESP_OK;
}

esp_err_t EpdPanel::DrawFramebufferStart(const uint8_t* fb, RefreshKind kind) {
  // Faithful port of GxEPD2's partial / full refresh sequences for
  // GDEY0213B74 / SSD1680. See GxEPD2_213_GDEY0213B74.cpp for every
  // command payload used below.
  //
  // Key GxEPD2 semantics we honour:
  //   * NO custom LUT upload. GxEPD2 relies on the OTP waveforms
  //     keyed by DUC2 = 0xd7 (fast full) / 0xfc (partial). The old v4
  //     code shipped a 153-byte LUT here and the DUC2=0xCC byte they
  //     chose to pair it with doesn't exist in GxEPD2 — that
  //     mismatch was the partial-refresh flakiness seen with
  //     invertedColor=false.
  //   * Partial writes only the 0x24 (BW) RAM. The controller copies
  //     post-activation into the internal "previous" state; the next
  //     partial diffs against that state automatically. Writing our
  //     software shadow to 0x26 before each partial (as the old code
  //     did) was actively poisoning the diff because the controller's
  //     internal previous-state is always correct whereas our shadow
  //     could drift on any partial the controller refused to latch.
  //   * Full refresh writes 0x24 then 0x26 both (mirrors
  //     writeImageForFullRefresh at GxEPD2_213_GDEY0213B74.cpp:60-64)
  //     so the OTP full waveform sees a consistent previous == current
  //     baseline and subsequent partials start from a known state.
  //   * Fast-full path: temperature-register override (0x1A = 0x64)
  //     plus DUC2 = 0xd7. GxEPD2_213_GDEY0213B74.cpp:376-387.
  auto* bus = cfg_.bus;
  auto& cs = cfg_.cs;
  last_kind_ = kind;

  // Per-refresh re-init mirrors v3_fci's display.init(0, false, 40)
  // before every refresh (epd.cpp:482). HW reset (RST# pulse) is
  // included because SW reset alone leaves enough internal controller
  // state intact that the shadow→0x26 priming below doesn't take
  // cleanly on the second-and-subsequent partials — adding HW reset
  // gives a known-good baseline every cycle. SSD1680 RAM is preserved
  // across HW reset (RST# only clears registers, not the static RAM
  // cells that hold 0x24 / 0x26), so the shadow→0x26 + fb→0x24
  // sequence below still ends up with the correct contents.
  HardReset();
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdSwReset), kTag, "swreset");
  WaitIdle(5000);
  ESP_RETURN_ON_ERROR(WriteInitCommands(), kTag, "init cmds");

  if (kind == RefreshKind::kPartial) {
    // Seed 0x26 (red RAM, used as "previous image" buffer for the
    // partial diff) with the last-displayed image. SSD1680's OTP
    // partial waveform (DUC2=0xFC) diffs 0x24-current vs 0x26-previous
    // and only drives changed pixels — but on the GDEY0213B74,
    // empirically the chip does NOT auto-copy 0x24→0x26 after the
    // partial activate (despite GxEPD2 assuming it does). The
    // mismatch surfaces as "1st partial after a full looks fine, 2nd
    // partial garbles": full writeImageForFullRefresh sets 0x26 =
    // full-image, so partial 1 diffs (new vs full) correctly; but
    // 0x26 stays stuck on full-image into partial 2 which then
    // computes (new2 vs full-image) = wrong diff. Writing shadow_
    // (our software copy of "what's currently on the panel", updated
    // at the end of this function after each successful refresh) to
    // 0x26 before every partial keeps the previous-buffer aligned
    // with the panel state. See pre-port commit 7355319^ for the
    // earlier v4 driver that already did this with DUC2=0xCC + a
    // custom 153-byte LUT; we keep the OTP DUC2=0xFC waveform and
    // add only the shadow→0x26 priming step.
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, shadow_), kTag,
                        "partial.red");
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "partial.bw");
    // Trigger sequence — _Update_Part (GxEPD2_213_GDEY0213B74.cpp:393-400):
    //   0x22 <- 0xFC   (DUC2: partial waveform)
    //   0x20           (activate)
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2,
                                         &kDuc2Partial, 1),
                        kTag, "duc2 partial");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate partial");
  } else {
    // Full refresh — writeImageForFullRefresh equivalent:
    //   0x26 <- fb    (previous-image RAM)
    //   0x24 <- fb    (current-image RAM)
    // GxEPD2 writes 0x26 first then 0x24; preserve that order so the
    // controller's internal state after activation is identical to
    // GxEPD2's.
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteRedRam, fb), kTag, "full.red");
    ESP_RETURN_ON_ERROR(WriteVram(kCmdWriteBwRam, fb), kTag, "full.bw");
    // _Update_Full slow path (GxEPD2_213_GDEY0213B74.cpp
    // useFastFullUpdate=false branch): DUC2=0xF7, activate.
    //
    // We used to use the fast-full path (0xD7 + temp-reg override) but
    // that waveform under-drove the white→black transition on
    // invertedColor=true content, leaving panels pale-gray instead of
    // true black. The slow waveform runs the full LUT sweep regardless
    // of previous/current pixel state and completes in ~1.5 s per
    // panel (vs. ~0.8 s for fast). Full refreshes only happen on
    // screen change / explicit /api/full_refresh / invertedColor
    // PATCH / boot; the 2× time cost is not user-visible on the
    // partial-refresh-dominated render path.
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdDispUpdateCtl2,
                                         &kDuc2FullSlow, 1),
                        kTag, "duc2 full");
    ESP_RETURN_ON_ERROR(bus->SendCommand(cs, kCmdActivateUpdate), kTag,
                        "activate full");
  }
  // Update the shadow — `fb` is now what the panel is displaying.
  // Shadow is kept because some caller paths still want to know the
  // last frame (e.g. log / diff tooling); the render-time semantics
  // no longer consume it.
  std::memcpy(shadow_, fb, FrameBytes());
  return ESP_OK;
}

esp_err_t EpdPanel::DrawFramebuffer(const uint8_t* fb, RefreshKind kind,
                                    uint32_t timeout_ms) {
  ESP_RETURN_ON_ERROR(DrawFramebufferStart(fb, kind), kTag, "start");
  return WaitForRefresh(timeout_ms);
}

}  // namespace btclock
