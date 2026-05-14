// Factory-reset + wifi-reset helpers. Both end in `esp_restart()`; the
// difference is scope.
//
// PerformFactoryReset wipes the NVS partition wholesale.
// Called from:
//   - POST /api/factory_reset (control_server.cpp), gated behind a
//     {"confirm":"ERASE"} body.
//   - The MCP1 "all four buttons held 5s" combo (buttons.cpp), which
//     fires the callback main.cpp wires in.
//
// PerformWifiReset removes only the STA credentials (`net/ssid`,
// `net/pw`) and clears `settings/wifiConfigured`, then reboots — the
// device comes back up in SoftAP provisioning mode while every other
// user setting (timezone, currencies, screens, LEDs, frontlight, NWC
// keys, ...) survives. The persisted SoftAP password (`net/app`) is
// preserved deliberately so a user who already wrote it down keeps a
// working portal credential after the reset.
// Called from:
//   - Boot-time button-1 hold (see app/boot/init_wifi_reset_button.*).
//     No HTTP surface — by design, the flash cost of an /api endpoint
//     wasn't justified on Rev A's already-tight partition.
//
// Both live in the settings component so call sites can reach them
// without pulling in main/. A UI-side splash must be rendered by the
// caller before invoking either: we can't depend on screen_manager
// without a circular include. The small delay inside each helper
// exists only so the EPD frame flush completes before `esp_restart()`.
//
// Both are non-returning — callers treat them as one-way trips.

#pragma once

namespace btclock {
namespace settings {

[[noreturn]] void PerformFactoryReset();
[[noreturn]] void PerformWifiReset();

}  // namespace settings
}  // namespace btclock
