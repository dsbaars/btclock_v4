// BTClock Rev B pin map. Keep the per-variant ifdefs in one place
// so the rest of the code never cares which board it's on.

#pragma once

#include <array>

#include "driver/gpio.h"

namespace btclock {
namespace board {

// --- I2C (MCP23017 + PCA9685 + BH1750) ---
constexpr gpio_num_t kI2cSda = GPIO_NUM_35;
constexpr gpio_num_t kI2cScl = GPIO_NUM_36;
constexpr uint16_t kMcp1Addr = 0x20;
constexpr uint16_t kPcaAddr = 0x42;
constexpr uint16_t kBhAddr = 0x5C;
constexpr gpio_num_t kPcaOe = GPIO_NUM_48;

// --- WS2812 NeoPixels ---
constexpr gpio_num_t kNeopixel = GPIO_NUM_15;
constexpr uint32_t kNeopixelCount = 4;

// --- EPD SPI bus (one host, 7 panels via software CS) ---
constexpr gpio_num_t kEpdSpiSclk = GPIO_NUM_12;
constexpr gpio_num_t kEpdSpiMosi = GPIO_NUM_11;
constexpr gpio_num_t kEpdDc = GPIO_NUM_14;

constexpr int kNumPanels = 7;
constexpr std::array<gpio_num_t, kNumPanels> kEpdCs = {
    GPIO_NUM_2,  GPIO_NUM_4,  GPIO_NUM_6,  GPIO_NUM_10,
    GPIO_NUM_38, GPIO_NUM_21, GPIO_NUM_17,
};
constexpr std::array<gpio_num_t, kNumPanels> kEpdBusy = {
    GPIO_NUM_3,  GPIO_NUM_5,  GPIO_NUM_7,  GPIO_NUM_9,
    GPIO_NUM_37, GPIO_NUM_18, GPIO_NUM_16,
};
constexpr std::array<uint8_t, kNumPanels> kEpdResetMcp = {
    8, 9, 10, 11, 12, 13, 14,
};

// --- Frontlight ---
// PCA9685 channels 1..kFrontlightChannelCount drive the per-panel backlight.
constexpr uint8_t kFrontlightChannelFirst = 1;
constexpr uint8_t kFrontlightChannelCount = 7;

// --- Optional peripherals (compile-time capability flags) ---
constexpr bool kHasFrontlight = true;    // PCA9685 @ kPcaAddr
constexpr bool kHasAmbientLight = true;  // BH1750 @ kBhAddr
constexpr bool kHasSecondMcp = false;    // V8 has a second MCP23017 @ 0x21
constexpr uint16_t kMcp2Addr = 0x00;
constexpr bool kHasMcpResetGpio = false;  // V8 gates MCP bootstrap via GPIO
constexpr gpio_num_t kMcpResetGpio = GPIO_NUM_NC;
// Address straps are hardwired on this variant; empty-array stubs so
// the `if constexpr (kHasMcpAddressGpios)` block in main() still
// resolves its identifier lookups even though the branch is discarded.
constexpr bool kHasMcpAddressGpios = false;
constexpr std::array<gpio_num_t, 0> kMcpAddressGpios = {};
constexpr std::array<bool, 0> kMcpAddressLevels = {};

// --- EPD pin routing (where CS / BUSY / RESET physically live) ---
constexpr PinSource kEpdCsSource = PinSource::kNative;
constexpr PinSource kEpdBusySource = PinSource::kNative;
constexpr PinSource kEpdResetSource = PinSource::kMcp1;

// --- Hardware orientation of LEDs and buttons ---
// Same wiring story as Rev A: the WS2812B chain's physical tail is the
// leftmost LED, and MCP1 GPA3 is the leftmost button label. See
// board_rev_a.hpp for the full rationale.
constexpr bool kLedChainReversed = true;
constexpr bool kButtonsInvertedDefault = false;

// --- Display-friendly hardware name ---
// Composes board + non-default panel suffix; 2.13" is the default for
// every board so the bare-board string stays stable for the common
// case.
#if defined(BTCLOCK_PANEL_2_9)
constexpr const char* kHardwareName = "Rev B 2.9\"";
#elif defined(BTCLOCK_PANEL_7_5)
constexpr const char* kHardwareName = "Rev B 7.5\"";
#else
constexpr const char* kHardwareName = "Rev B";
#endif

// Machine-readable identifier emitted as /api/settings.hwRev — see
// board_rev_a.hpp for the full rationale. Underscore form matches the
// WebUI's `firmwareBinaryMap` / `webuiBinaryMap` lookup keys.
#if defined(BTCLOCK_PANEL_2_9)
constexpr const char* kHardwareId = "REV_B_EPD_2_9";
#elif defined(BTCLOCK_PANEL_7_5)
constexpr const char* kHardwareId = "REV_B_EPD_7_5";
#else
constexpr const char* kHardwareId = "REV_B_EPD_2_13";
#endif

}  // namespace board
}  // namespace btclock
