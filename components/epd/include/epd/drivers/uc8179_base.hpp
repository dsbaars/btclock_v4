// Shared UC8179 base. Today only the GDEY075T7 (7.5", 800x480)
// derives from this; the IC family also covers the GDEY075Z08
// (3-color 7.5") which we don't ship.
//
// UNTESTED — reach a working board before relying on this. The
// implementation follows GxEPD2_750_GDEY075T7.cpp register-for-
// register but has not been driven against real silicon in this
// firmware. Differences from the SSD1680 base worth flagging:
//   * Power management is explicit — 0x04 (power on) / 0x02 (power
//     off) are real commands, not just "turn on the analog block".
//   * The framebuffer write command is 0x13 (current) / 0x10
//     (previous), not 0x24 / 0x26.
//   * Refresh is triggered by 0x12 (display refresh), not 0x22 +
//     0x20.
//   * Partial-update windowing uses 0x90; we don't currently pack
//     anything smaller than the full panel into a single command.

#pragma once

#include <cstdint>

#include "epd/panel.hpp"
#include "esp_err.h"

namespace btclock {
namespace epd {

class Uc8179Base : public IEpdPanel {
 public:
  explicit Uc8179Base(const PanelConfig& cfg);
  ~Uc8179Base() override;

  esp_err_t Init() override;
  esp_err_t DrawFramebufferStart(const uint8_t* fb,
                                 RefreshKind kind = RefreshKind::kFull) override;
  esp_err_t WaitForRefresh(uint32_t timeout_ms = 10'000) override;
  bool IsIdle() const override;

 protected:
  // 7.5" GDEY075T7 packs 800/8 = 100 bytes per scan-line, NOT the
  // 16-byte landscape stride the screen renderers default to today.
  // Concrete subclasses override Stride() — but every existing
  // renderer assumes Stride()==16 so painting a 7.5" with the v4
  // text-tower layout would overflow VRAM. Mark this in the driver
  // and leave the Stride() override on the concrete subclass.

  void HardReset();
  void WaitIdle(uint32_t timeout_ms);
  uint32_t BusyPollMs() const;
  esp_err_t PowerOn();
  esp_err_t PowerOff();

  PanelConfig cfg_;
  bool power_is_on_ = false;
};

}  // namespace epd
}  // namespace btclock
