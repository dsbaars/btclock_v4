// Pure-virtual EPD panel interface. Concrete drivers live under
// drivers/ — gdey0213b74.{hpp,cpp} (SSD1680, 122×250 visible),
// gdey029t94.{hpp,cpp} (SSD1680, 128x296), gdey075t7.{hpp,cpp}
// (UC8179, 800x480 — un-tested scaffold).
//
// The factory (epd::CreatePanel) returns one unique_ptr<IEpdPanel>
// per CS line; multi-panel boards (V8) hold N independent driver
// instances, all bound to the same EpdBus + DC line.

#pragma once

#include <cstdint>

#include "epd/bus.hpp"
#include "esp_err.h"

namespace btclock {

enum class RefreshKind : uint8_t {
  kFull,
  kPartial,
};

namespace epd {

// Shared bring-up + render API. The screens code (main/screens/*.cpp,
// main/app/screen_manager.*) reaches the panel only through this
// interface, so we can swap drivers without touching the renderers.
class IEpdPanel {
 public:
  virtual ~IEpdPanel() = default;

  IEpdPanel(const IEpdPanel&) = delete;
  IEpdPanel& operator=(const IEpdPanel&) = delete;

  // Hardware reset + initial controller bring-up. Allocates the
  // shadow framebuffer in PSRAM on first call.
  virtual esp_err_t Init() = 0;

  // Kick off a refresh: write VRAM, send activation, return
  // immediately. The caller must call WaitForRefresh() (or use the
  // blocking DrawFramebuffer wrapper) before re-using `fb`.
  virtual esp_err_t DrawFramebufferStart(
      const uint8_t* fb, RefreshKind kind = RefreshKind::kFull) = 0;

  // Block on BUSY going idle. Returns ESP_ERR_TIMEOUT if the panel
  // doesn't finish within `timeout_ms`. Default mirrors the legacy
  // epd::IEpdPanel API the boot/provisioning UI calls without an arg.
  virtual esp_err_t WaitForRefresh(uint32_t timeout_ms = 10'000) = 0;

  // Non-blocking BUSY check. False while the panel is refreshing.
  virtual bool IsIdle() const = 0;

  virtual int Width() const = 0;
  virtual int Height() const = 0;
  // Bytes per scan-line as packed by the renderer. Every shipping
  // panel in v4 packs at 8 px/byte rounded up to 16 — this stride is
  // shared by the WASM shim and every renderer
  // (font.hpp::SetPixelLandscape). 16 bytes covers up to 128 px wide;
  // the 7.5" panel overrides Stride() (100 bytes) but the renderers
  // don't yet support strides ≠ 16.
  virtual int Stride() const { return kStride; }
  int FrameBytes() const { return Stride() * Height(); }
  // Legacy compile-time stride. Many renderers reference
  //   panels[i]->kStride
  // and the framebuffer storage type
  //   uint8_t fb[N][16 * 296]
  // bakes the 16 in directly. Keep the constexpr around for those
  // call sites; new code should call Stride() so the 7.5" override
  // can take effect once the renderers learn dynamic strides.
  static constexpr int kStride = 16;

  // Convenience blocking helper. Default impl is just Start + Wait.
  esp_err_t DrawFramebuffer(const uint8_t* fb,
                            RefreshKind kind = RefreshKind::kFull,
                            uint32_t timeout_ms = 10'000) {
    esp_err_t err = DrawFramebufferStart(fb, kind);
    if (err != ESP_OK) return err;
    return WaitForRefresh(timeout_ms);
  }

 protected:
  IEpdPanel() = default;
};

// Same `Config` shape every concrete driver consumes. Pin sources
// are caller-resolved (PinSource is a board-level type), not part of
// the EPD layer.
struct PanelConfig {
  EpdBus* bus = nullptr;
  EpdIoPin cs;
  EpdIoPin busy;
  EpdIoPin reset;
};

// Global polarity switch — when true, every VRAM byte is XOR'd with
// 0xFF before SPI DMA so renderers can keep producing 1=white,
// 0=black framebuffers regardless of the user's invertedColor pref.
// Read at boot from settings/invertedColor and re-installed via
// PATCH /api/settings; the on_inverted_color_changed hook also
// MarkDirty()s the screen so the next paint picks the new polarity.
void SetGlobalInverted(bool inverted);
bool GetGlobalInverted();

// When enabled, partial refreshes skip the per-frame
// HardReset + SwReset + WriteInitCommands sequence inside
// Ssd1680Base::DrawFramebufferStart. Saves ~80 ms/frame at the cost
// of relying on the chip's registers staying valid between refreshes
// — safe under continuous repeated partials on a quiet bus (boot
// spinner). kFull always re-inits regardless of this flag, so any
// later full refresh recovers a known state.
void SetGlobalFastPartial(bool enable);
bool GetGlobalFastPartial();

}  // namespace epd
}  // namespace btclock
