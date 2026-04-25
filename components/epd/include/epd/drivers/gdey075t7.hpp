// GDEY075T7 — 7.5" UC8179, 800x480.
// Authoritative GxEPD2 reference: src/gdey/GxEPD2_750_GDEY075T7.cpp.
//
// UNTESTED — reach a working board before relying on this. The
// driver code is a faithful port of the GxEPD2 init / refresh
// sequence, but no part of the firmware has been driven against
// real silicon yet. The factory currently rejects
// BTCLOCK_PANEL=7_5 + BTCLOCK_BOARD=V8 at CMake time; the other
// combinations will compile and link but every screen renderer
// assumes a 16-byte landscape stride and would clip the 7.5" frame.

#pragma once

#include "epd/drivers/uc8179_base.hpp"

namespace btclock {
namespace epd {

class Gdey075T7 : public Uc8179Base {
 public:
  using Uc8179Base::Uc8179Base;

  int Width() const override { return 800; }
  int Height() const override { return 480; }
  // Stride per scan-line is 800 / 8 = 100 bytes. See header comment
  // — the renderers don't actually accommodate this yet.
  int Stride() const override { return 100; }
};

}  // namespace epd
}  // namespace btclock
