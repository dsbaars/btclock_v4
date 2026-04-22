// Board dispatcher. The build system defines exactly one of
// POC_BOARD_REV_A, POC_BOARD_REV_B, or POC_BOARD_V8; this header routes
// to the right pin map. Shared types that every board header references
// are defined here first so the variant headers don't need to agree on
// them independently.

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

#if defined(POC_BOARD_REV_A)
#include "board_rev_a.hpp"
#elif defined(POC_BOARD_REV_B)
#include "board_rev_b.hpp"
#elif defined(POC_BOARD_V8)
#include "board_v8.hpp"
#else
#error "No POC_BOARD variant selected — pass -DPOC_BOARD=REV_A, REV_B or V8"
#endif
