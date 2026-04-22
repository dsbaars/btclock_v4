// BTClock V8 pin map (custom board, 8 panels, 16 MB flash).
//
// V8 is the odd one: EPD CS and RESET both live on MCP23017 #2, BUSY
// lives on MCP23017 #1, and a single native GPIO (kMcpResetGpio) drives
// the reset line of both expanders at boot. No PCA9685 or BH1750.

#pragma once

#include <array>

#include "driver/gpio.h"

namespace btclock {
namespace board {

// --- I2C (two MCP23017 expanders — no PCA9685, no BH1750 on V8) ---
constexpr gpio_num_t kI2cSda = GPIO_NUM_1;
constexpr gpio_num_t kI2cScl = GPIO_NUM_2;
constexpr uint16_t kMcp1Addr = 0x20;     // buttons + BUSY inputs
constexpr uint16_t kMcp2Addr = 0x21;     // EPD CS + RESET outputs
// Sentinel values for unused peripheral addresses / pins.
constexpr uint16_t kPcaAddr = 0x00;
constexpr uint16_t kBhAddr = 0x00;
constexpr gpio_num_t kPcaOe = GPIO_NUM_NC;

// --- WS2812 NeoPixels ---
constexpr gpio_num_t kNeopixel = GPIO_NUM_5;
constexpr uint32_t kNeopixelCount = 4;

// --- EPD SPI bus (one host, 8 panels via MCP-multiplexed CS) ---
constexpr gpio_num_t kEpdSpiSclk = GPIO_NUM_12;
constexpr gpio_num_t kEpdSpiMosi = GPIO_NUM_11;
constexpr gpio_num_t kEpdDc = GPIO_NUM_38;

constexpr int kNumPanels = 8;
// CS on MCP2 — pin indices 0..15.
constexpr std::array<uint8_t, kNumPanels> kEpdCs = {
    8, 10, 12, 14, 0, 2, 4, 6,
};
// BUSY on MCP1.
constexpr std::array<uint8_t, kNumPanels> kEpdBusy = {
    8, 9, 10, 11, 12, 13, 14, 4,
};
// RESET on MCP2.
constexpr std::array<uint8_t, kNumPanels> kEpdResetMcp = {
    9, 11, 13, 15, 1, 3, 5, 7,
};

// --- Frontlight (absent on V8) ---
constexpr uint8_t kFrontlightChannelFirst = 0;
constexpr uint8_t kFrontlightChannelCount = 0;

// --- Optional peripherals ---
constexpr bool kHasFrontlight = false;
constexpr bool kHasAmbientLight = false;
constexpr bool kHasSecondMcp = true;
// V8 pulses both MCP reset lines via a native GPIO at startup before
// any I2C traffic. The MCP23017 datasheet requires a >= 5 µs low pulse
// on RESET and at least one µs of setup before I2C, which we round up
// generously (5 ms) since it's a one-shot boot step.
constexpr bool kHasMcpResetGpio = true;
constexpr gpio_num_t kMcpResetGpio = GPIO_NUM_21;

// --- EPD pin routing (everything on the expanders) ---
constexpr PinSource kEpdCsSource = PinSource::kMcp2;
constexpr PinSource kEpdBusySource = PinSource::kMcp1;
constexpr PinSource kEpdResetSource = PinSource::kMcp2;

constexpr const char* kHardwareName = "V8";

}  // namespace board
}  // namespace btclock
