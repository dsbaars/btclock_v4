// Boot-time wifi-reset hardware path.
//
// Polls MCP1 pin 0 (physical "button 1" — leftmost) for a 3 s continuous
// hold immediately after MCP bring-up. If the button stays held through
// the whole window, calls settings::PerformWifiReset() which clears the
// STA credentials and reboots into SoftAP provisioning. Released early
// → returns silently and normal boot continues.
//
// LED feedback: while the hold is being tracked, the NeoPixel ring runs
// the kFlashError red effect so the user can tell the device has noticed
// them. No EPD splash — at this point in boot the panels may still be
// painting BTCLOCK and re-binding the framebuffer to a single character
// would cost ~1.5 s of refresh time the user can't see anyway (they're
// looking at the button, not the panels). The NVS wipe + reboot is
// instantaneous from the user's point of view.
//
// Replaces the v3 firmware behaviour where holding button 1 at power-up
// dropped the device back into WiFiManager's AP. v4 preserves every
// other user setting — only `net/ssid`, `net/pw` and `wifiConfigured`
// are removed.

#pragma once

namespace btclock {

struct AppCtx;

// Must be called AFTER InitHardware (so the MCP is reachable) and
// BEFORE InitNetwork (so the wifi creds we're about to wipe haven't
// been read yet). Cheap when the button isn't held: a single I2C read
// + early return.
void MaybeWifiResetAtBoot(AppCtx& ctx);

}  // namespace btclock
