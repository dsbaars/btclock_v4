// Pre-AppCtx boot prologue: LED task + DND suppressor + boot banner.
//
// Runs before AppCtx is constructed because the LED task owns its own
// queue/state behind a namespace singleton (main/io/led_controller.cpp)
// — it doesn't fit on AppCtx, and we want the boot rainbow / suppressor
// wired as early as possible so the user sees the LEDs react even if a
// later init step hangs.
//
// Also emits the ESP_LOGI boot banner (firmware name + PSRAM / heap
// sizes) because that's the first visible line of the log and belongs
// next to the LED boot effect.

#pragma once

namespace btclock {

void InitBootLeds();

}  // namespace btclock
