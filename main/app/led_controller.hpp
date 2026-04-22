// NeoPixel controller — FreeRTOS task + event queue.
//
// Callers post LedEvents from any task (button handler, data-source
// callback, main loop); a dedicated task consumes the queue and drives
// the WS2812B strip. Matches the event-queue pattern the production
// firmware uses for the same purpose.

#pragma once

#include <cstdint>

#include "driver/gpio.h"

namespace btclock {

enum class LedEvent : uint8_t {
  kSetBoot,     // rainbow cycle across the strip
  kSetIdle,     // LEDs off
  kBlockFlash,  // ~5 Hz orange blink for 4 s, auto-returns to kIdle
};

// Create the event queue and start the LED task. Call once at boot;
// subsequent calls are undefined. No shutdown path — the task runs
// until reboot.
void InitLeds(gpio_num_t pin, uint32_t count);

// Post an event to the LED task. Thread-safe. Non-blocking; drops if
// the queue is full (8 slots).
void PostLedEvent(LedEvent ev);

}  // namespace btclock
