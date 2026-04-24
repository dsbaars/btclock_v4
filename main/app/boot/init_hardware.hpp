// I2C bus + port expanders + PWM + ambient-light sensor.
//
// Brings the on-board I2C peripherals to life in the order they need:
//   1. I2C bus.
//   2. (V8 only) drive the 6 MCP address-strap GPIOs + pulse the
//      shared MCP RESET line so the two MCP23017s latch to distinct
//      addresses.
//   3. MCP23017 #1 (buttons on P0..P3, optional EPD reset port for
//      Rev A/B on P8..P14).
//   4. (V8 only) MCP23017 #2 (EPD reset port).
//   5. (frontlight boards only) PCA9685 + FrontlightController; the
//      controller's DND suppressor is armed before Start() so an
//      active schedule suppresses the boot fade-in.
//   6. (ambient-light boards only) BH1750 + LightSensor; a missing
//      sensor logs a warning and continues with IsAvailable()=false.
//
// Every field lands on AppCtx. Safe to call exactly once, at boot,
// from the main task.

#pragma once

namespace btclock {

struct AppCtx;

void InitHardware(AppCtx& ctx);

}  // namespace btclock
