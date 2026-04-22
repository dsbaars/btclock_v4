// Provisioning-mode render pass.
//
// Mirrors the production firmware's WiFiManager setAPCallback layout
// (src/lib/system/config.cpp). Explicit N=7 and N=8 instantiations cover
// Rev A/B and V8 respectively; additional counts would need new
// instantiations in provisioning_ui.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>

#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"

namespace btclock {

template <size_t N>
void RenderProvisioningScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& ap_ssid,
    const std::string& ap_pw);

}  // namespace btclock
