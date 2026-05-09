// GDEY0213B74 — 2.13" SSD1680. Visible ink is 122×250 px; each RAM row is
// 16 bytes (128 bits). Width()/Height() match the visible lattice — layout,
// preview WS headers, and SetPixelLandscape clamping stay WYSIWYG. VRAM
// writes still ship Stride()*Height() bytes (see
// Ssd1680Base::SetPartialRamArea: w=122 and w=128 both byte-span columns
// 0..15).

#pragma once

#include "epd/drivers/ssd1680_base.hpp"

namespace btclock {
namespace epd {

class Gdey0213B74 : public Ssd1680Base {
 public:
  using Ssd1680Base::Ssd1680Base;

  int Width() const override { return 122; }
  int Height() const override { return 250; }
};

}  // namespace epd
}  // namespace btclock
