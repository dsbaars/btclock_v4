// Frontlight controller — FreeRTOS task + event queue driving the
// PCA9685 per-panel backlight channels.
//
// Mirrors the old firmware's `LedHandler::frontlight*` API as a sibling
// to `app/led_controller` (the NeoPixel state machine). Kept separate
// because the two hardware subsystems are unrelated: NeoPixel WS2812
// strip vs. 12-bit PWM on the I2C expander.
//
// Effects in scope (beads btclock_v3_fci-7ma):
//   - smooth fade between any two brightness targets
//   - "block-flash" pulse: fade-up -> hold -> fade-down
//   - "zap-flash" pulse: same shape, longer hold (matches old firmware
//     where flash-on-update and flash-on-zap share the code path)
//   - ambient auto-off driven by BH1750 lux threshold
//
// NOT in scope here: /api/frontlight/* HTTP wiring (lives on a separate
// branch and will call the C++ API surface at the bottom of this file
// when the branches merge), NeoPixel effects (see btclock_v3_fci-fxh),
// NVS persistence of brightness/threshold prefs (see btclock_v3_fci-jwz).

#pragma once

#include <cstdint>

#include "app/frontlight_fader.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pca9685.hpp"

namespace btclock {

// --- Tuning knobs (single source of truth) -------------------------
//
// Values match the old Arduino firmware's defaults in
// src/lib/system/defaults.hpp + src/lib/drivers/leds/led_handler.cpp
// so Rev-B hardware behaves the same after the IDF port. Until NVS
// settings land (see btclock_v3_fci-jwz), the controller reads these
// at construction and exposes runtime setters for threshold + enable.
namespace frontlight {

// PCA9685 is 12-bit; keep max at the old firmware's default of 2048
// (about half-duty) rather than 4095 — the backlight is bright enough
// at 2048 and full-duty wastes current.
constexpr uint16_t kDefaultMaxDuty = 2048;

// Per-tick step, in 12-bit duty units. Must be > 0. At 15 ms/tick and
// step=25, a full 0 -> 2048 fade takes ~1.2 s, matching old firmware.
constexpr uint16_t kFadeStep = 25;

// Controller task tick period, ms. Matches DEFAULT_FL_EFFECT_DELAY.
constexpr uint32_t kTickMs = 15;

// Ambient-light threshold, lux. Below this, frontlight stays on;
// above, it fades off. Matches DEFAULT_LUX_LIGHT_TOGGLE.
constexpr uint32_t kDefaultLuxThreshold = 128;

// Pulse-effect timings (ms). Old firmware chains fadeIn+fadeOut back
// to back with no explicit hold — we add a tiny hold so a "flash"
// isn't imperceptible at faster fade steps. Block and zap share
// shape; zap lingers longer for higher visibility on low-frequency
// events.
constexpr uint32_t kBlockFlashHoldMs = 120;
constexpr uint32_t kZapFlashHoldMs = 400;

}  // namespace frontlight

// --- Controller (FreeRTOS task) ------------------------------------
//
// Initialised once on boards where `kHasFrontlight` is true.
// Events are enqueued from any task (data-source callback, control
// server, BH1750 consumer); the controller task drains them, updates
// the fader's target, and on each tick writes every PCA9685 channel
// in the frontlight range to the interpolated duty.
//
// The same duty is written to every channel — the old firmware's
// staggered per-channel cascade is a cosmetic detail we intentionally
// drop for now (adds complexity without user-visible value on the
// 7-panel Rev B, where all channels share one diffuser).
enum class FrontlightEvent : uint8_t {
  kOn,           // fade to current configured brightness
  kOff,          // fade to 0
  kSetBrightness,  // fade to payload brightness, also updates configured value
  kBlockFlash,   // pulse up -> hold -> return to previous state
  kZapFlash,     // same shape, longer hold
};

// Event payload. Only `kSetBrightness` uses `value`; other events
// ignore it.
struct FrontlightCommand {
  FrontlightEvent event;
  uint16_t value;  // 0..max_duty for kSetBrightness
};

class FrontlightController {
 public:
  // Must outlive the controller. The PCA9685's Begin() should already
  // have been called.
  FrontlightController(Pca9685& pca, uint8_t channel_first,
                        uint8_t channel_count);

  // Creates the event queue + task. Call once after construction.
  void Start();

  // Thread-safe. Non-blocking; drops silently if the queue is full.
  void Post(FrontlightCommand cmd);

  // Convenience wrappers for the HTTP wiring on the sibling branch.
  void On() { Post({FrontlightEvent::kOn, 0}); }
  void Off() { Post({FrontlightEvent::kOff, 0}); }
  void SetBrightness(uint16_t duty) {
    Post({FrontlightEvent::kSetBrightness, duty});
  }
  void Flash() { Post({FrontlightEvent::kBlockFlash, 0}); }
  void ZapFlash() { Post({FrontlightEvent::kZapFlash, 0}); }

  // --- Runtime-configurable ambient-light behaviour ---
  // Lux threshold and enable flag are plain getters/setters; NVS
  // persistence is deferred to btclock_v3_fci-jwz.
  void SetAmbientAutoOff(bool enabled) { ambient_enabled_ = enabled; }
  bool ambient_auto_off() const { return ambient_enabled_; }

  void SetLuxThreshold(uint32_t lux) { lux_threshold_ = lux; }
  uint32_t lux_threshold() const { return lux_threshold_; }

  // Feed the latest ambient-light reading. Safe to call from any task;
  // enqueues kOn/kOff as needed. No-op when ambient_auto_off() is
  // false, or when the reading is < 0 (sensor error sentinel).
  void OnAmbientLux(float lux);

  // --- Status surface for future /api/frontlight/status wiring ---
  struct Status {
    bool enabled;
    uint16_t current_duty;
    uint16_t target_duty;
    uint16_t configured_brightness;
    uint32_t lux_threshold;
    bool ambient_auto_off;
  };
  Status GetStatus() const;

 private:
  static void TaskTrampoline(void* arg);
  void TaskLoop();
  void WriteAllChannels(uint16_t duty);

  Pca9685& pca_;
  uint8_t channel_first_;
  uint8_t channel_count_;

  QueueHandle_t queue_ = nullptr;
  FrontlightFader fader_;

  // User-configured brightness (what kOn resumes to). Distinct from
  // the fader's current/target so a flash doesn't clobber it.
  uint16_t configured_brightness_ = frontlight::kDefaultMaxDuty;
  bool logical_on_ = false;

  // Ambient-light coupling state. Updated from OnAmbientLux() on the
  // caller's task; read from the controller task. Simple atomics would
  // be tidier but we only need eventual consistency here.
  volatile bool ambient_enabled_ = true;
  volatile uint32_t lux_threshold_ = frontlight::kDefaultLuxThreshold;
};

}  // namespace btclock
