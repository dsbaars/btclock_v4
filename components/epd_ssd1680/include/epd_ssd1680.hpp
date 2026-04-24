#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "mcp23017.hpp"

namespace btclock {

class EpdPanel;

// A single digital I/O line, backed by either a native MCU GPIO or an
// MCP23017 expander pin. Introduced for V8 where EPD CS and RESET both
// live on an I2C expander — earlier Rev A/B boards always used native
// GPIO. Native writes are single-cycle register hits; MCP writes carry
// ~1 ms of I2C per toggle, so callers that poll (BUSY) should slow down
// when is_mcp() is true.
class EpdIoPin {
 public:
  EpdIoPin() = default;

  static EpdIoPin Native(gpio_num_t pin) {
    EpdIoPin p;
    p.kind_ = Kind::kNative;
    p.native_ = pin;
    return p;
  }
  static EpdIoPin Mcp(Mcp23017* mcp, uint8_t pin) {
    EpdIoPin p;
    p.kind_ = Kind::kMcp;
    p.mcp_ = mcp;
    p.mcp_pin_ = pin;
    return p;
  }

  bool is_native() const { return kind_ == Kind::kNative; }
  bool is_mcp() const { return kind_ == Kind::kMcp; }

  // Runtime debug string, e.g. "GPIO14" or "mcp.8". Returns a pointer to
  // a per-call static buffer, so don't hold across calls.
  const char* Describe() const;

  // One-shot init: direction + initial level.
  esp_err_t ConfigureAsOutput(bool initial_high);
  esp_err_t ConfigureAsInput();

  esp_err_t Write(bool high);
  bool Read() const;

 private:
  enum class Kind : uint8_t { kNative, kMcp };
  Kind kind_ = Kind::kNative;
  gpio_num_t native_ = GPIO_NUM_NC;
  Mcp23017* mcp_ = nullptr;
  uint8_t mcp_pin_ = 0;
};

class EpdBus {
 public:
  EpdBus(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi,
         gpio_num_t dc, uint32_t clk_hz = 4 * 1000 * 1000,
         int max_transfer_bytes = 8 * 1024);
  ~EpdBus();

  EpdBus(const EpdBus&) = delete;
  EpdBus& operator=(const EpdBus&) = delete;

  esp_err_t SendCommand(EpdIoPin& cs, uint8_t cmd,
                        const uint8_t* params = nullptr, size_t nparams = 0);
  esp_err_t SendData(EpdIoPin& cs, const uint8_t* data, size_t len);

 private:
  spi_host_device_t host_;
  spi_device_handle_t dev_ = nullptr;
  gpio_num_t dc_;
};

enum class PanelKind : uint8_t {
  k2_13,   // 122 x 250
  k2_9,    // 128 x 296
};

enum class RefreshKind : uint8_t {
  kFull,
  kPartial,
};

// One SSD1680 e-paper panel.
//
// The API is split into non-blocking Start + Wait so a caller can fan
// out a batch of refreshes across all 7 panels and wait on them in
// parallel: the per-panel controller refresh (~800 ms partial, ~2.3 s
// full) is the dominant cost, and runs concurrently once activation
// has been dispatched. Shared-SPI writes still serialise.
//
// Global polarity switch — SetGlobalInverted() toggles an every-byte
// XOR 0xFF applied during VRAM writes so the physical pixels flip from
// black-on-white (default) to white-on-black without any renderer
// change. Read at boot from `settings/invertedColor` and flipped via
// PATCH /api/settings; callers should MarkDirty on ScreenManager so
// the next paint does a full refresh with the new polarity.
void EpdSetGlobalInverted(bool inverted);
bool EpdGetGlobalInverted();

class EpdPanel {
 public:
  struct Config {
    EpdBus* bus = nullptr;
    EpdIoPin cs;
    EpdIoPin busy;
    EpdIoPin reset;
    PanelKind kind = PanelKind::k2_13;
  };

  explicit EpdPanel(const Config& cfg);
  ~EpdPanel();

  EpdPanel(const EpdPanel&) = delete;
  EpdPanel& operator=(const EpdPanel&) = delete;

  // Hardware reset + init. Allocates the shadow framebuffer in PSRAM
  // on first call (~4 KB per 2.13" panel).
  esp_err_t Init();

  // Kick off a refresh: write VRAM, send activation, return immediately.
  // The caller must call WaitForRefresh() (or the blocking
  // DrawFramebuffer wrapper) before re-using `fb`.
  esp_err_t DrawFramebufferStart(const uint8_t* fb,
                                 RefreshKind kind = RefreshKind::kFull);

  // Block on BUSY going idle. Returns ESP_ERR_TIMEOUT if the panel
  // doesn't finish within `timeout_ms`.
  esp_err_t WaitForRefresh(uint32_t timeout_ms = 10'000);

  // Blocking convenience: Start + Wait.
  esp_err_t DrawFramebuffer(const uint8_t* fb,
                            RefreshKind kind = RefreshKind::kFull,
                            uint32_t timeout_ms = 10'000);

  // Non-blocking BUSY check. False while the panel is refreshing.
  bool IsIdle() const;

  int Width() const;
  int Height() const;
  static constexpr int kStride = 16;
  int FrameBytes() const { return kStride * Height(); }

 private:
  void HardReset();
  void WaitIdle(uint32_t timeout_ms);
  uint32_t BusyPollMs() const;
  esp_err_t WriteInitCommands();
  esp_err_t RewindRam();
  esp_err_t WriteVram(uint8_t write_cmd, const uint8_t* fb);

  Config cfg_;
  // Shadow of the frame currently on the panel. Kept for debug /
  // future diff tooling; the render-time path no longer writes it
  // back to the controller — the port to the GxEPD2 reference driver
  // relies on the controller's internal previous-frame copy for
  // partial refreshes (GxEPD2_213_GDEY0213B74.cpp) rather than a
  // software shadow.
  uint8_t* shadow_ = nullptr;
  // Scratch buffer used when the global invertedColor flag is set —
  // WriteVram() inverts `fb` byte-for-byte into this buffer before
  // the SPI DMA. PSRAM-allocated on first use so the normal (non-
  // inverted) path pays no memory cost.
  uint8_t* invert_scratch_ = nullptr;
  // Recorded by DrawFramebufferStart so WaitForRefresh knows whether
  // to send the explicit power-off (only after partial — full
  // implicitly powers off via the OTP waveform).
  RefreshKind last_kind_ = RefreshKind::kFull;
};

}  // namespace btclock
