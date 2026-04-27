// GDEY0213B74 — 2.13" SSD1680, 122x250 visible (128 addressable wide).

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
