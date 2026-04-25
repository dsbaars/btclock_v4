// BTClock Rev A pin map (Lolin S3 Mini, no frontlight, no BH1750).
// See production firmware platformio.ini env lolin_s3_mini_*.

#pragma once

#include <array>

#include "driver/gpio.h"

namespace btclock {
namespace board {

// --- I2C (MCP23017 only — no PCA9685, no BH1750 on Rev A) ---
constexpr gpio_num_t kI2cSda = GPIO_NUM_35;
constexpr gpio_num_t kI2cScl = GPIO_NUM_36;
constexpr uint16_t kMcp1Addr = 0x20;
// Sentinel values so the dropped branches of `if constexpr (kHas*)` still
// compile. They are never read at runtime because the flags below are false.
constexpr uint16_t kPcaAddr = 0x00;
constexpr uint16_t kBhAddr = 0x00;
constexpr gpio_num_t kPcaOe = GPIO_NUM_NC;

// --- WS2812 NeoPixels ---
constexpr gpio_num_t kNeopixel = GPIO_NUM_34;
constexpr uint32_t kNeopixelCount = 4;

// --- EPD SPI bus (one host, 7 panels via software CS) ---
constexpr gpio_num_t kEpdSpiSclk = GPIO_NUM_12;
constexpr gpio_num_t kEpdSpiMosi = GPIO_NUM_11;
constexpr gpio_num_t kEpdDc = GPIO_NUM_14;

constexpr int kNumPanels = 7;
// Same as Rev B except index 4 — Rev A uses GPIO 33 there (Rev B GPIO 38).
constexpr std::array<gpio_num_t, kNumPanels> kEpdCs = {
    GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_6, GPIO_NUM_10,
    GPIO_NUM_33, GPIO_NUM_21, GPIO_NUM_17,
};
constexpr std::array<gpio_num_t, kNumPanels> kEpdBusy = {
    GPIO_NUM_3, GPIO_NUM_5, GPIO_NUM_7, GPIO_NUM_9,
    GPIO_NUM_37, GPIO_NUM_18, GPIO_NUM_16,
};
constexpr std::array<uint8_t, kNumPanels> kEpdResetMcp = {
    8, 9, 10, 11, 12, 13, 14,
};

// --- Frontlight (absent) ---
constexpr uint8_t kFrontlightChannelFirst = 0;
constexpr uint8_t kFrontlightChannelCount = 0;

// --- Optional peripherals ---
constexpr bool kHasFrontlight = false;
constexpr bool kHasAmbientLight = false;
constexpr bool kHasSecondMcp = false;
constexpr uint16_t kMcp2Addr = 0x00;
constexpr bool kHasMcpResetGpio = false;
constexpr gpio_num_t kMcpResetGpio = GPIO_NUM_NC;
// Address straps are hardwired on this variant, but the main() boot
// path references kMcpAddressGpios / kMcpAddressLevels inside an
// `if constexpr (kHasMcpAddressGpios)` guard. `if constexpr` still
// requires identifier lookup on the discarded branch, so empty arrays
// are the cleanest no-op stub (the range-for compiles into nothing).
constexpr bool kHasMcpAddressGpios = false;
constexpr std::array<gpio_num_t, 0> kMcpAddressGpios = {};
constexpr std::array<bool, 0> kMcpAddressLevels = {};

// --- EPD pin routing (same as Rev B: native CS/BUSY, RESET on MCP1) ---
constexpr PinSource kEpdCsSource = PinSource::kNative;
constexpr PinSource kEpdBusySource = PinSource::kNative;
constexpr PinSource kEpdResetSource = PinSource::kMcp1;

// Human-readable name composes board + non-default panel. 2.13" is
// the default for every board so we elide the suffix in that case;
// the WebUI/mDNS TXT layer disambiguates same-board different-panel
// units via this string.
#if defined(BTCLOCK_PANEL_2_9)
constexpr const char* kHardwareName = "Rev A 2.9\"";
#elif defined(BTCLOCK_PANEL_7_5)
constexpr const char* kHardwareName = "Rev A 7.5\"";
#else
constexpr const char* kHardwareName = "Rev A";
#endif

}  // namespace board
}  // namespace btclock
