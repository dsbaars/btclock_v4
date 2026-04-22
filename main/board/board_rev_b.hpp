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
    GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_6, GPIO_NUM_10,
    GPIO_NUM_38, GPIO_NUM_21, GPIO_NUM_17,
};
constexpr std::array<gpio_num_t, kNumPanels> kEpdBusy = {
    GPIO_NUM_3, GPIO_NUM_5, GPIO_NUM_7, GPIO_NUM_9,
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
constexpr bool kHasFrontlight = true;   // PCA9685 @ kPcaAddr
constexpr bool kHasAmbientLight = true; // BH1750 @ kBhAddr
constexpr bool kHasSecondMcp = false;   // V8 has a second MCP23017 @ 0x21
constexpr uint16_t kMcp2Addr = 0x00;
constexpr bool kHasMcpResetGpio = false; // V8 gates MCP bootstrap via GPIO
constexpr gpio_num_t kMcpResetGpio = GPIO_NUM_NC;

// --- EPD pin routing (where CS / BUSY / RESET physically live) ---
constexpr PinSource kEpdCsSource = PinSource::kNative;
constexpr PinSource kEpdBusySource = PinSource::kNative;
constexpr PinSource kEpdResetSource = PinSource::kMcp1;

// --- Display-friendly hardware name ---
constexpr const char* kHardwareName = "Rev B";

}  // namespace board
}  // namespace btclock
