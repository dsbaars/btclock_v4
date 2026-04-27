// GDEY029T94 — 2.9" SSD1680, 128x296.
//
// Differences from the 2.13" GDEY0213B74:
//   * Driver output control uses 0x27 0x01 0x00 (HEIGHT-1 = 0x0127)
//     vs the 2.13"'s 0xF9 0x00 0x00 (250-1 = 0xF9).
//   * The reference driver enables fast-full update by default
//     (useFastFullUpdate=true → DUC2=0xD7 + temp-reg 0x1A=0x64). We
//     keep the slow-full path (0xF7) for parity with the 2.13"
//     behaviour: the slow waveform survives the invertedColor=true
//     case where fast-full under-drove the white→black transition
//     and left panels pale-gray. Re-enable fast-full per-driver if
//     a future bring-up confirms it works for our use.
//   * The GDEY029T94 reference does NOT prime 0x26 with the previous
//     frame before each partial — the chip auto-copies 0x24→0x26 on
//     activation. Disable the 2.13"'s shadow→0x26 workaround.

#pragma once

#include "epd/drivers/ssd1680_base.hpp"

namespace btclock {
namespace epd {

class Gdey029T94 : public Ssd1680Base {
 public:
  using Ssd1680Base::Ssd1680Base;

  int Width() const override { return 128; }
  int Height() const override { return 296; }

 protected:
  esp_err_t WriteDriverOutputControl() override;
  // 2.9" auto-copies 0x24→0x26 on activate, so no shadow priming.
  bool PrimePartialPreviousRam() const override { return false; }
};

}  // namespace epd
}  // namespace btclock
