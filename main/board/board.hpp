// Board dispatcher. The build system defines exactly one of
// BTCLOCK_BOARD_REV_A, BTCLOCK_BOARD_REV_B, or BTCLOCK_BOARD_V8; this
// header routes to the right pin map. Shared types that every board
// header references are defined here first so the variant headers don't
// need to agree on them independently.
//
// Panel geometry is orthogonal to the board: -DBTCLOCK_PANEL=2_13|2_9|7_5
// from the top-level CMakeLists.txt picks BTCLOCK_PANEL_<X>=1, which
// the EPD driver factory consumes (components/epd/src/factory.cpp).
// Every board × panel combo is allowed; the per-board EPD pin map
// (CS/DC/RST/BUSY) is panel-agnostic — geometry lives entirely on the
// driver side, so swapping panels is a one-flag rebuild.

#pragma once

#include <cstdint>

namespace btclock {
namespace board {

// Where a digital line lives. Used to decide whether an EpdIoPin is a
// native GPIO, on the primary MCP23017 (MCP1), or on the secondary one
// (MCP2, only present on V8).
enum class PinSource : uint8_t { kNative, kMcp1, kMcp2 };

}  // namespace board
}  // namespace btclock

#if defined(BTCLOCK_BOARD_REV_A)
#include "board_rev_a.hpp"
#elif defined(BTCLOCK_BOARD_REV_B)
#include "board_rev_b.hpp"
#elif defined(BTCLOCK_BOARD_V8)
#include "board_v8.hpp"
#else
#error \
    "No BTCLOCK_BOARD variant selected — pass -DBTCLOCK_BOARD=REV_A, REV_B or V8"
#endif
