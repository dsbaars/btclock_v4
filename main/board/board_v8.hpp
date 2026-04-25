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

// V8's MCP23017 address-strap pins (A0..A2 on each chip) are wired
// to ESP32 GPIOs rather than hardwired resistors. Firmware MUST drive
// these high/low before releasing RESET, otherwise both chips default
// to 0x20 and collide on the bus (symptom: I2C scan finds one device,
// second MCP NACKs every transaction). Values 000 → 0x20 (mcp1) and
// 001 → 0x21 (mcp2) match the Arduino-era firmware's setupMcp().
constexpr bool kHasMcpAddressGpios = true;
constexpr std::array<gpio_num_t, 6> kMcpAddressGpios = {
    GPIO_NUM_6,  GPIO_NUM_7,  GPIO_NUM_8,    // mcp1 A0, A1, A2
    GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_14,   // mcp2 A0, A1, A2
};
constexpr std::array<bool, 6> kMcpAddressLevels = {
    false, false, false,                      // mcp1: 000 → 0x20
    true,  false, false,                      // mcp2: 001 → 0x21
};

// --- EPD pin routing (everything on the expanders) ---
constexpr PinSource kEpdCsSource = PinSource::kMcp2;
constexpr PinSource kEpdBusySource = PinSource::kMcp1;
constexpr PinSource kEpdResetSource = PinSource::kMcp2;

// Composes board + non-default panel suffix; 2.13" is the default so
// the bare-board string stays stable for the common case.
#if defined(BTCLOCK_PANEL_2_9)
constexpr const char* kHardwareName = "V8 2.9\"";
#elif defined(BTCLOCK_PANEL_7_5)
constexpr const char* kHardwareName = "V8 7.5\"";
#else
constexpr const char* kHardwareName = "V8";
#endif

}  // namespace board
}  // namespace btclock
