#include "io/frontlight_stagger.hpp"

namespace btclock {

namespace {

// v3 used `int` for dutyCycle and the offset arithmetic; we widen to
// int32_t so the intermediate subtraction can't wrap before the clamp.
int32_t ClampDuty(int32_t value, int32_t max_brightness) {
  if (value < 0) return 0;
  if (value > max_brightness) return max_brightness;
  return value;
}

}  // namespace

uint16_t ComputeStaggeredDuty(uint32_t tick, uint8_t led_index,
                              uint8_t num_leds, uint16_t max_brightness,
                              uint16_t step, StaggerDirection direction) {
  if (num_leds == 0 || step == 0 || led_index >= num_leds) return 0;
  if (max_brightness == 0) return 0;

  const int32_t max_b = static_cast<int32_t>(max_brightness);
  const int32_t n = static_cast<int32_t>(num_leds);
  const int32_t phase_step = max_b / n;  // v3: `max/N` integer division

  // The outer-loop counter v3 used (dutyCycle) advances by `step` per
  // tick starting at 0 for kIn, at max_brightness for kOut. Reproduce
  // that exactly so the stagger offsets line up 1:1 with the old visual.
  int32_t duty_cycle = 0;
  int32_t offset = 0;
  switch (direction) {
    case StaggerDirection::kIn: {
      duty_cycle = static_cast<int32_t>(tick) * step;
      offset = static_cast<int32_t>(led_index) * phase_step;
      break;
    }
    case StaggerDirection::kOut: {
      // v3 started at max_brightness and stepped down. Starting value
      // must be exactly max_brightness on tick 0 (not max-step) so the
      // last-lit-LED (index 0) holds at max until its countdown starts.
      duty_cycle =
          max_b - static_cast<int32_t>(tick) * static_cast<int32_t>(step);
      offset = (n - 1 - static_cast<int32_t>(led_index)) * phase_step;
      break;
    }
  }

  const int32_t led_brightness = ClampDuty(duty_cycle - offset, max_b);
  return static_cast<uint16_t>(led_brightness);
}

uint32_t StaggerTotalTicks(uint8_t num_leds, uint16_t max_brightness,
                           uint16_t step) {
  if (num_leds == 0 || step == 0 || max_brightness == 0) return 0;
  const int32_t max_b = static_cast<int32_t>(max_brightness);
  const int32_t n = static_cast<int32_t>(num_leds);
  // v3's outer bound: dutyCycle <= max + (N-1) * max/N
  // Number of iterations = floor(bound / step) + 1 (inclusive loop).
  const int32_t bound = max_b + (n - 1) * (max_b / n);
  return static_cast<uint32_t>(bound / static_cast<int32_t>(step)) + 1u;
}

uint32_t StaggerDelayMs(uint32_t effect_delay_ms, uint8_t num_leds) {
  if (num_leds == 0) return effect_delay_ms;
  const uint32_t delay = effect_delay_ms / num_leds;
  return delay == 0 ? 1u : delay;
}

}  // namespace btclock
