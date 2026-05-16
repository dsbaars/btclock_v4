// Pure-logic state machine that decides what the frontlight should do
// in response to a fresh ambient-lux reading.
//
// Split out of FrontlightController so the hysteresis + user-off +
// off-when-dark interactions can be covered by host tests (which have
// no FreeRTOS / ESP-IDF). The controller holds one instance and feeds
// it every sample; the returned action is the only I/O contract:
//
//   kNone  — do nothing, the current target is correct
//   kOn    — fade up to the configured brightness (ambient-driven on)
//   kOff   — fade to 0 (ambient-driven off, distinct from user-off)
//
// The controller never calls the user-facing On()/Off() helpers from
// here; it enqueues dedicated kAmbientOn/kAmbientOff events so a
// user-initiated /api/frontlight/off remains authoritative until the
// user (or a flash event) reverses it.
//
// Behaviour mirrors the v3 Arduino firmware's handleFrontlight() in
// src/main.cpp:31-47 but adds two things v3 lacked:
//
//   1. A "user-off" latch — `/api/frontlight/off` sets it and the
//      policy then refuses to auto-on until cleared.
//   2. Hysteresis around the off-when-dark threshold — v3 fades in/out
//      on each sample, flapping at the boundary. We use a 1.0 lux
//      enter / 2.0 lux exit band.

#pragma once

#include <cstdint>

namespace btclock {

struct FrontlightAmbientConfig {
  // Master switch. When false the policy returns kNone for every
  // sample (matches v3's `luxLightToggle == 0 -> feature disabled`).
  bool ambient_auto_enabled = true;

  // Lux at which the normal ambient-on/ambient-off threshold flips.
  // Below this -> fade in, above this -> fade out. Matches v3's
  // `luxLightToggle` NVS pref (DEFAULT_LUX_LIGHT_TOGGLE = 128).
  uint32_t lux_threshold = 128;

  // Extra "dark mode" behaviour. When true and the room is very dark
  // (lux < kDarkEnterLux) the policy forces the frontlight off and
  // keeps it off until lux rises past kDarkExitLux. Mirrors v3's
  // `flOffWhenDark` pref (src/main.cpp:38).
  bool off_when_dark = false;
};

// Hysteresis band for the off-when-dark latch. Intentionally tight —
// v3 compares against `lightLevel <= 1` with no hysteresis at all, so
// any non-zero band is already an improvement. 1.0/2.0 keeps the room-
// dark call stable under the ±0.1 lux noise of a BH1750 in H-res mode.
inline constexpr float kDarkEnterLux = 1.0f;
inline constexpr float kDarkExitLux = 2.0f;

enum class FrontlightAmbientAction : uint8_t {
  kNone,
  kOn,
  kOff,
};

class FrontlightAmbientPolicy {
 public:
  FrontlightAmbientPolicy() = default;

  void SetConfig(const FrontlightAmbientConfig& cfg) { cfg_ = cfg; }
  const FrontlightAmbientConfig& config() const { return cfg_; }

  // Latched when the user posts `/api/frontlight/off`. Cleared by an
  // explicit user-on, a brightness write, or a flash/zap pulse. The
  // policy treats this as "never auto-on"; ambient-off is still allowed
  // because it's idempotent when already off.
  void SetUserOff(bool on) { user_off_ = on; }
  bool user_off() const { return user_off_; }

  // Observable — tests use this to pin the hysteresis contract.
  bool is_dark() const { return dark_; }

  // True if the frontlight is currently being driven by the renderer
  // (last action was kOn) — lets the policy emit a clean kOff when the
  // user asks for auto-off mid-session. Defaults to false (boot is
  // dark / off) and must be kept in sync by the caller via
  // NoteOutputOn / NoteOutputOff.
  void NoteOutputOn() { output_on_ = true; }
  void NoteOutputOff() { output_on_ = false; }
  bool output_on() const { return output_on_; }

  // Feed the latest ambient reading. Negative lux = sensor error /
  // unavailable — returns kNone without updating dark-mode state.
  FrontlightAmbientAction Evaluate(float lux);

 private:
  FrontlightAmbientConfig cfg_{};
  bool user_off_ = false;
  bool dark_ = false;
  bool output_on_ = false;
};

}  // namespace btclock
