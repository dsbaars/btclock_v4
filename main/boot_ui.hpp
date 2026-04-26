// Boot-time splash screen: one letter of BTCLOCK per panel, displayed
// once at boot so it's obvious when the device is coming up (the
// NeoPixel rainbow already gives a lit-up signal; this gives a
// per-panel confirmation that the SPI bus + all 7/8 displays are alive).

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"

namespace btclock {

template <size_t N>
void RenderSplashScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                        uint8_t (&fb_storage)[N][16 * 296],
                        const AppFonts& fonts);

}  // namespace btclock
