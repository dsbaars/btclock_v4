// Factory-reset helper. Wipes the NVS partition wholesale and reboots.
//
// Called from:
//   - POST /api/factory_reset (control_server.cpp), gated behind a
//     {"confirm":"ERASE"} body.
//   - The MCP1 "all four buttons held 5s" combo (buttons.cpp), which
//     fires the callback main.cpp wires in.
//
// Lives in the settings component so both call sites can reach it
// without pulling in main/. The UI-side "Resetting…" splash must be
// rendered by the caller before invoking this: we can't depend on
// screen_manager without a circular include. The small delay below
// exists only so the EPD frame flush completes before `esp_restart()`.
//
// Non-returning — callers treat the function as a one-way trip.

#pragma once

namespace btclock {
namespace settings {

[[noreturn]] void PerformFactoryReset();

}  // namespace settings
}  // namespace btclock
