#include "frontlight_logic/frontlight_ambient_policy.hpp"

namespace btclock {

FrontlightAmbientAction FrontlightAmbientPolicy::Evaluate(float lux) {
  // Sensor-error sentinel. Don't touch dark-mode latch; the room's
  // light may well be unchanged, the sensor just failed this read.
  if (lux < 0.0f) return FrontlightAmbientAction::kNone;

  // Master gate. v3 maps `luxLightToggle == 0` to "feature off", and
  // we preserve that.
  if (!cfg_.ambient_auto_enabled || cfg_.lux_threshold == 0) {
    return FrontlightAmbientAction::kNone;
  }

  // Off-when-dark takes priority over the normal threshold. Hysteresis
  // band is small on purpose — the sensor is stable in a steady
  // environment and we only need a buffer against sampling noise.
  if (cfg_.off_when_dark) {
    if (dark_) {
      if (lux >= kDarkExitLux) {
        dark_ = false;  // fall through to the normal threshold logic
      } else {
        // Stay latched off while still dark. If we were emitting light
        // for any reason, issue a kOff so the backlight actually fades.
        if (output_on_) return FrontlightAmbientAction::kOff;
        return FrontlightAmbientAction::kNone;
      }
    } else if (lux < kDarkEnterLux) {
      dark_ = true;
      return output_on_ ? FrontlightAmbientAction::kOff
                        : FrontlightAmbientAction::kNone;
    }
  } else {
    // Feature just got disabled — release the latch so next sample
    // treats the world as non-dark.
    dark_ = false;
  }

  // Normal ambient-auto path. Mirrors v3 main.cpp:40-44:
  //   lux < threshold && !on -> fade in
  //   lux > threshold &&  on -> fade out
  // Equality is a no-op on purpose, so a boundary-grazing sensor
  // doesn't flap every sample.
  const uint32_t lux_u = static_cast<uint32_t>(lux < 0.0f ? 0.0f : lux);
  if (lux_u < cfg_.lux_threshold) {
    if (user_off_) return FrontlightAmbientAction::kNone;
    if (!output_on_) return FrontlightAmbientAction::kOn;
  } else if (lux_u > cfg_.lux_threshold) {
    if (output_on_) return FrontlightAmbientAction::kOff;
  }
  return FrontlightAmbientAction::kNone;
}

}  // namespace btclock
