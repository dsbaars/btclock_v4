// Pure-logic single-channel linear fader.
//
// Split out from frontlight_controller.hpp so the interpolator math
// can be covered by host tests (test_host/ has no ESP-IDF / FreeRTOS
// headers available). The controller consumes one instance per
// frontlight group.
//
// Semantics:
//   - current() and target() live in [0, max_duty].
//   - SetTarget() clamps; negative -> 0, > max_duty -> max_duty.
//   - Step() moves current toward target by at most `step`, returning
//     the new current. At the target Step() is idempotent.
//   - Full-travel rule (pinned for host tests):
//       For max_duty M, step S, starting at 0 with target M, the
//       fader reaches M at exactly tick ceil(M / S) (not ceil + 1).
//       That matches the old firmware's fade loop, which also takes
//       ceil(M / S) iterations.

#pragma once

#include <cstdint>

namespace btclock {

class FrontlightFader {
 public:
  explicit FrontlightFader(uint16_t max_duty, uint16_t step);

  // Clamps to [0, max_duty].
  void SetTarget(int32_t target);

  // One tick. Returns new current brightness.
  uint16_t Step();

  // Teleport — skips the interpolation, useful on boot / force-off.
  void Snap(int32_t value);

  uint16_t current() const { return current_; }
  uint16_t target() const { return target_; }
  bool AtTarget() const { return current_ == target_; }
  uint16_t max_duty() const { return max_duty_; }

 private:
  uint16_t max_duty_;
  uint16_t step_;
  uint16_t current_ = 0;
  uint16_t target_ = 0;
};

}  // namespace btclock
