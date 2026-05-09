// Shared SSD1680 base for the GDEY0213B74 (2.13", 122×250 visible) and
// GDEY029T94 (2.9", 128x296). The two panels share the SSD1680
// command set; only the driver-output-control / RAM-area parameters
// and a few init details differ. The base captures the bring-up
// sequence, RAM cursor / window setup, and the partial/full activate
// flow; subclasses override only the geometry- and waveform-specific
// pieces.

#pragma once

#include <cstdint>

#include "epd/panel.hpp"
#include "esp_err.h"

namespace btclock {
namespace epd {

class Ssd1680Base : public IEpdPanel {
 public:
  explicit Ssd1680Base(const PanelConfig& cfg);
  ~Ssd1680Base() override;

  esp_err_t Init() override;
  esp_err_t DrawFramebufferStart(
      const uint8_t* fb, RefreshKind kind = RefreshKind::kFull) override;
  esp_err_t WaitForRefresh(uint32_t timeout_ms = 10'000) override;
  bool IsIdle() const override;

 protected:
  // Geometry hooks. Concrete drivers report their pixel dimensions
  // (Width / Height) plus the byte-aligned RAM addressable width
  // (RamWidthBytes) — for both shipped 2.13"/2.9" panels this is 16.
  virtual int RamWidthBytes() const { return 16; }

  // Per-driver pieces of the init sequence. The base runs SW reset,
  // then calls WriteDriverOutputControl + WriteBorder, then sets up
  // the partial-RAM area for the full panel before returning. Default
  // border = 0x05 (2.13" GDEY0213B74 datasheet); the 2.9" GDEY029T94
  // also uses 0x05, so subclasses typically only override the driver-
  // output-control bytes.
  virtual esp_err_t WriteDriverOutputControl();
  virtual uint8_t BorderWaveform() const { return 0x05; }

  // 0x21 (display update control 1) payload. Default 0x00, 0x80 from
  // both reference drivers — bit 7 of byte 1 selects "RAM content
  // only, no red". Override only if a driver needs different bits.
  virtual void DispUpdateControl1(uint8_t* dst /*[2]*/) const {
    dst[0] = 0x00;
    dst[1] = 0x80;
  }

  // 0x22 + 0x20 sequence selectors. Defaults match the GDEY0213B74
  // reference: 0xF7 slow-full, 0xFC partial. The fast-full variant
  // (0xD7 + temp-reg override 0x1A=0x64) is opt-in via UseFastFullUpdate().
  virtual uint8_t Duc2Full() const { return 0xF7; }
  virtual uint8_t Duc2Partial() const { return 0xFC; }
  virtual bool UseFastFullUpdate() const { return false; }

  // Whether this driver primes RAM 0x26 with the previous frame
  // before each partial activate. The 2.13" GDEY0213B74 needs this
  // (the chip doesn't auto-copy 0x24→0x26 after a partial activate);
  // the 2.9" GDEY029T94 doesn't, so the 2.9" driver disables the
  // prime.
  virtual bool PrimePartialPreviousRam() const { return true; }

  // Shared helpers.
  void HardReset();
  void WaitIdle(uint32_t timeout_ms);
  uint32_t BusyPollMs() const;
  esp_err_t WriteInitCommands();
  esp_err_t SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  esp_err_t WriteVram(uint8_t write_cmd, const uint8_t* fb);

  PanelConfig cfg_;
  uint8_t* shadow_ = nullptr;
  uint8_t* invert_scratch_ = nullptr;
  RefreshKind last_kind_ = RefreshKind::kFull;
};

}  // namespace epd
}  // namespace btclock
