#include "app/frontlight_fader.hpp"

namespace btclock {

FrontlightFader::FrontlightFader(uint16_t max_duty, uint16_t step)
    : max_duty_(max_duty), step_(step == 0 ? 1 : step) {}

void FrontlightFader::SetTarget(int32_t target) {
  if (target < 0) target = 0;
  if (target > max_duty_) target = max_duty_;
  target_ = static_cast<uint16_t>(target);
}

void FrontlightFader::Snap(int32_t value) {
  SetTarget(value);
  current_ = target_;
}

uint16_t FrontlightFader::Step() {
  if (current_ == target_) return current_;
  if (current_ < target_) {
    const uint32_t next = static_cast<uint32_t>(current_) + step_;
    current_ = (next >= target_) ? target_ : static_cast<uint16_t>(next);
  } else {
    const int32_t next = static_cast<int32_t>(current_) - step_;
    current_ = (next <= target_) ? target_ : static_cast<uint16_t>(next);
  }
  return current_;
}

}  // namespace btclock
