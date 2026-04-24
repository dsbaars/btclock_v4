// Pure-logic per-LED duty computation for the staggered frontlight
// flash animation.
//
// Mirrors the v3 Arduino firmware's `LedHandler::frontlightFadeInAll` /
// `frontlightFadeOutAll` staggered path
// (btclock_v3_fci: src/lib/drivers/leds/led_handler.cpp:573-646) and
// is called by FrontlightController's pulse state machine.
//
// Why a helper: the controller's state machine lives inside a FreeRTOS
// task and the PCA9685 write; the duty math itself is pure, so pulling
// it out lets host tests cover the ordering / clamping / total-tick
// budget without dragging in esp_timer + the I2C stack.
//
// Semantics (pinned for host tests — DO NOT "tidy up" the ordering
// without a matching test update; the visual cascade direction is what
// users remember from v3):
//
//   Direction::kIn  (dark -> bright):
//     LED index 0 ramps first, N-1 ramps last. At tick t (0-based):
//       duty[i] = clamp(t*step - i * max/N, 0, max)
//
//   Direction::kOut (bright -> dark):
//     LED index N-1 darkens first, 0 darkens last. At tick t (0-based):
//       duty[i] = clamp((total_ticks - 1 - t)*step - (N-1-i) * max/N, 0, max)
//
//   Both directions complete in the same total tick count:
//       total_ticks = ceil((max + (N-1) * max/N) / step) + 1
//   This matches v3's outer loop bound `dutyCycle <= max + (N-1)*max/N`.
//
//   Per-tick wall delay (caller's responsibility):
//       stagger_delay_ms = flEffectDelay / N
//   So a full flash (in + out) takes ~2 * total_ticks * stagger_delay_ms.

#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {

enum class StaggerDirection : uint8_t {
  kIn,   // fade from 0 toward max_brightness across all LEDs
  kOut,  // fade from max_brightness toward 0 across all LEDs
};

// Computed duty in [0, max_brightness] for one LED on one tick.
//
// Integer-math only — matches v3 which used `int` throughout the fade
// loops. `led_index` must be < num_leds; `num_leds` must be > 0;
// `step` must be > 0. Returns 0 for any invalid input so callers can
// stay branch-free.
uint16_t ComputeStaggeredDuty(uint32_t tick, uint8_t led_index,
                              uint8_t num_leds, uint16_t max_brightness,
                              uint16_t step, StaggerDirection direction);

// Total tick count for one direction. Callers loop tick in [0,
// TotalTicks()) then stop — at TotalTicks() - 1, kIn lands every LED
// on max_brightness and kOut lands every LED on 0.
uint32_t StaggerTotalTicks(uint8_t num_leds, uint16_t max_brightness,
                            uint16_t step);

// Per-tick wall delay. Floor division matches v3 — for flEffectDelay=15
// and N=7 that's 2ms (the extra 1ms is absorbed in the visible cadence).
// Clamped to >= 1 so a too-small effect_delay still advances.
uint32_t StaggerDelayMs(uint32_t effect_delay_ms, uint8_t num_leds);

}  // namespace btclock
