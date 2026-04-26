// Factory-reset splash + NVS wipe + reboot.
//
// Shared by two call paths:
//   1. POST /api/factory_reset → ControlServer::Config::on_factory_reset
//   2. Hardware combo: buttons 0..3 held for 5 s → SetOnAllButtonsLongPress
//
// Both land on the same function so the user-visible behaviour is
// identical regardless of how the reset was triggered: paint an
// "ERASING" splash onto the EPD chain, then call
// settings::PerformFactoryReset() which zeros NVS and reboots.
//
// Kept [[noreturn]] — PerformFactoryReset() never returns and nothing
// after its call is reachable.

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "board/board.hpp"
#include "data_core/hub.hpp"
#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"

namespace btclock {

class ScreenManager;

// Paint the farewell splash and trigger the wipe. The panel array must
// be the board's configured kNumPanels (AppCtx::panels); fb_storage
// must be the corresponding kNumPanels × 16·296 buffer.
[[noreturn]] void DoFactoryReset(
    ScreenManager& sm,
    std::array<std::unique_ptr<EpdPanel>, btclock::board::kNumPanels>& panels,
    uint8_t (&fb_storage)[btclock::board::kNumPanels][16 * 296],
    const AppFonts& fonts, DataHub* hub);

}  // namespace btclock
