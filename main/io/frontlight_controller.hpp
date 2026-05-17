// Frontlight controller — FreeRTOS task + event queue driving the
// PCA9685 per-panel backlight channels.
//
// Mirrors the old firmware's `LedHandler::frontlight*` API as a sibling
// to `app/led_controller` (the NeoPixel state machine). Kept separate
// because the two hardware subsystems are unrelated: NeoPixel WS2812
// strip vs. 12-bit PWM on the I2C expander.
//
// Effects in scope:
//   - smooth fade between any two brightness targets
//   - "block-flash" pulse: fade-up -> hold -> fade-down
//   - "zap-flash" pulse: same shape, longer hold (matches old firmware
//     where flash-on-update and flash-on-zap share the code path)
//   - ambient auto-off driven by BH1750 lux threshold
//
// NOT in scope here: /api/frontlight/* HTTP wiring (lives on a separate
// branch and will call the C++ API surface at the bottom of this file
// when the branches merge), NeoPixel effects, NVS persistence of
// brightness/threshold prefs.

#pragma once

// Header is variant-gated. On boards without a PCA9685 backlight the
// class isn't defined; consumers must wrap any use in
// `#if BTCLOCK_HAS_FRONTLIGHT` and the file isn't included at all on
// Rev A / V8.
#if BTCLOCK_HAS_FRONTLIGHT

#include <cstdint>
#include <functional>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "frontlight_logic/frontlight_ambient_policy.hpp"
#include "frontlight_logic/frontlight_fader.hpp"
#include "frontlight_logic/frontlight_stagger.hpp"
#include "pca9685.hpp"

namespace btclock {

// --- Tuning knobs (single source of truth) -------------------------
//
// Values match the old Arduino firmware's defaults in
// src/lib/system/defaults.hpp + src/lib/drivers/leds/led_handler.cpp
// so Rev-B hardware behaves the same after the IDF port. Until NVS
// settings land, the controller reads these
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

// Default for the user-tunable `flEffectDelay` NVS pref — the outer
// wall-clock period (ms) that a full staggered fade-in or fade-out
// targets. Per-LED stagger delay is this divided by the LED count.
// Matches v3 Arduino DEFAULT_FL_EFFECT_DELAY.
constexpr uint32_t kDefaultEffectDelayMs = 15;

// Ambient-light threshold, lux. Below this, frontlight stays on;
// above, it fades off. Matches DEFAULT_LUX_LIGHT_TOGGLE.
constexpr uint32_t kDefaultLuxThreshold = 128;

// Pulse-effect "hold" between the two halves of a staggered flash.
// v3 chained fadeIn+fadeOut back-to-back with zero hold — we honour
// that for block-flash to keep parity, and give zap-flash a small
// visible hold (users notice rare zap events more than common block
// events). Held in the bright state when the pre-flash target was
// dark, held in the dark state when the pre-flash target was bright.
constexpr uint32_t kBlockFlashHoldMs = 0;
constexpr uint32_t kZapFlashHoldMs = 250;

}  // namespace frontlight

// --- Controller (FreeRTOS task) ------------------------------------
//
// Initialised once on boards where `kHasFrontlight` is true.
// Events are enqueued from any task (data-source callback, control
// server, BH1750 consumer); the controller task drains them, updates
// the fader's target, and on each tick writes every PCA9685 channel
// in the frontlight range to the interpolated duty.
//
// Steady-state fades write the same duty to every channel. The
// block-flash / zap-flash pulse uses a staggered per-channel cascade
// — restored after the initial IDF port dropped it — so the flash
// animation matches v3 Arduino's visual signature (LED index 0 leads
// on the fade-in half, index N-1 lags; reversed on the fade-out half).
// See frontlight_logic/frontlight_stagger.hpp for the pure math.
enum class FrontlightEvent : uint8_t {
  kOn,          // user-on: clears user-off latch, fades to configured
  kOff,         // user-off: sets user-off latch, fades to 0
  kAmbientOn,   // ambient-loop on: no-op if user-off latch is set
  kAmbientOff,  // ambient-loop off: fades to 0, does NOT set user-off latch
  kDarkOff,     // off-when-dark off: like kAmbientOff but bypasses flAlwaysOn
  kSetBrightness,  // fade to payload brightness, also updates configured value
  kBlockFlash,     // pulse up -> hold -> return to previous state
  kZapFlash,       // same shape, longer hold
  kSetChannelDuties,  // manual per-channel write — payload comes from a
                      // controller-internal staging area (kept off the
                      // queue entry so FrontlightCommand stays compact).
                      // Cancels any pulse + snaps the fader so the
                      // pattern persists until the next event.
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

  // Manual per-channel write. Cancels any in-flight pulse + snaps the
  // fader to the first channel's value so the pattern persists until
  // another event (kOn/kOff/kFlash/kSet/kAmbient*) overrides it. `count`
  // is clamped to channel_count_; entries past 8 are dropped. Used by
  // the /api/frontlight/set debug endpoint and tests.
  void SetChannelDuties(const uint16_t* duties, uint8_t count);

  // --- Runtime-configurable ambient-light behaviour ---
  // Forwarded to the embedded policy. `ambient_auto_off` matches the
  // v3 semantic where `luxLightToggle == 0` disables the feature
  // entirely — useful for Rev-A / V8 (no sensor), but still reachable
  // via NVS on Rev-B for users who prefer a fixed brightness.
  void SetAmbientAutoOff(bool enabled);
  bool ambient_auto_off() const;

  void SetLuxThreshold(uint32_t lux);
  uint32_t lux_threshold() const;

  // `flOffWhenDark` — when true and the BH1750 reports effective-zero
  // lux, the controller forces the backlight off and ignores
  // auto-on/threshold logic until the room lights come back up.
  // Matches v3 Arduino firmware src/main.cpp:38.
  void SetOffWhenDark(bool enabled);
  bool off_when_dark() const;

  // Max duty (0..4095) written to the PCA9685 for the "on" state.
  // Read from NVS `flMaxBrightness` at boot and live-tunable.
  void SetConfiguredBrightness(uint16_t duty);

  // Per-flash outer cadence in ms — NVS `flEffectDelay`. Governs the
  // speed of the staggered block/zap-flash animation. Live-tunable;
  // read by the controller task at the start of each pulse so a PATCH
  // to `flEffectDelay` is picked up on the next flash without reboot.
  void SetEffectDelay(uint32_t ms) {
    // Plain store, read from the controller task on the next flash
    // start. Eventual consistency is fine — a racing PATCH during an
    // in-flight pulse just takes effect on the pulse after it.
    effect_delay_ms_ = ms;
  }
  uint32_t effect_delay_ms() const { return effect_delay_ms_; }

  // `flDisable` — hard mute. When true, Post() drops anything that
  // would light the panel and queues a kOff so an already-on backlight
  // fades to black. Wins over flAlwaysOn (matches the v3 semantics
  // documented in btclock_v4-63p: disable forces off).
  void SetDisabled(bool disabled) { disabled_ = disabled; }
  bool disabled() const { return disabled_; }

  // `flAlwaysOn` — pin the backlight on regardless of the regular
  // ambient threshold. When true, kAmbientOff is dropped in Post() so
  // the BH1750 loop can't fade the panel out at high lux. The
  // off-when-dark feature is a more specific user override and is
  // routed via kDarkOff, which is NOT gated here — a user who enables
  // both flAlwaysOn and flOffWhenDark expects the dark-mode branch to
  // win in pitch black. Disabled() still wins over both.
  void SetAlwaysOn(bool always_on) { always_on_ = always_on; }
  bool always_on() const { return always_on_; }

  // `flFlashOnUpd` — gate the Flash() (kBlockFlash) path. When false,
  // Post() silently drops kBlockFlash so block/data updates don't
  // pulse the backlight. kZapFlash is not gated here — that goes
  // through flFlashOnZap, evaluated at the zap-listener call site.
  void SetFlashOnUpdate(bool enabled) { flash_on_update_ = enabled; }
  bool flash_on_update() const { return flash_on_update_; }

  // `flOffOnDnd` — gate the active-suppressor predicate. When true
  // (default), an active DND window suppresses the frontlight (Post
  // drops on/flash events and fades the panel out via kAmbientOff).
  // When false, the suppressor predicate's return value is ignored
  // and the frontlight stays under its normal user/ambient control,
  // so the LED ring can be muted by DND independently of the panel.
  void SetOffOnDnd(bool enabled) { off_on_dnd_ = enabled; }
  bool off_on_dnd() const { return off_on_dnd_; }

  // Install a predicate the controller consults before acting on Post.
  // When true AND `off_on_dnd_` is true (the default), kOn /
  // kSetBrightness / kBlockFlash / kZapFlash are silently dropped and
  // an immediate kOff is enqueued so the backlight fades to black.
  // Pointed at the DND subsystem from main.cpp; std::function keeps
  // the dnd component out of the frontlight controller's include
  // graph. Thread-safety: predicate is swapped atomically; callers
  // that set this more than once race on interleaved updates (not a
  // concern — only wired at boot).
  void SetActiveSuppressor(std::function<bool()> predicate) {
    suppressor_ = std::move(predicate);
  }

  // Feed the latest ambient-light reading. Safe to call from any task;
  // enqueues kAmbientOn / kAmbientOff as needed. No-op when
  // ambient_auto_off() is false, when the reading is < 0 (sensor
  // error sentinel), or when the user-off latch is set and the policy
  // would otherwise turn the backlight on.
  void OnAmbientLux(float lux);

  // Edge-triggered DND re-evaluation. Post() only gates *new* commands
  // when DND is active; an already-on backlight stays on until something
  // pushes a new event. Call this on a periodic tick (1 Hz from the
  // main loop is enough for time-based DND minute boundaries) so the
  // panel fades off when DND turns on and resumes when it turns off,
  // even with no other events flowing. Tracks both the suppressor's
  // value and `flOffOnDnd` so toggling the opt-out also lifts/applies
  // suppression without a reboot.
  void OnDndStateMaybeChanged();

  // --- Status surface for future /api/frontlight/status wiring ---
  struct Status {
    bool enabled;
    // Aggregate fader state — useful as a single "where is the bank
    // headed" number even though each channel can transiently differ
    // during a staggered flash. `current_duty` matches duties[0] in
    // steady state.
    uint16_t current_duty;
    uint16_t target_duty;
    uint16_t configured_brightness;
    uint32_t lux_threshold;
    bool ambient_auto_off;
    // Per-channel duty mirror — one entry per panel-backlight LED.
    // Cap matches the largest variant (V8: 8 channels). `channel_count`
    // is the filled prefix; only the first `channel_count` entries are
    // populated. Mirrored directly off WriteAllChannels /
    // WriteStaggeredTick so a flash-in-progress shows the correct
    // staggered shape rather than a single aggregate.
    uint16_t duties[8] = {0};
    uint8_t channel_count = 0;
  };
  Status GetStatus() const;

 private:
  static void TaskTrampoline(void* arg);
  void TaskLoop();
  void WriteAllChannels(uint16_t duty);
  // Drive one tick of the staggered flash animation. Bypasses the
  // fader so each LED sees its phase-shifted duty directly.
  void WriteStaggeredTick(uint32_t tick, uint16_t max_brightness,
                          StaggerDirection direction);

  Pca9685& pca_;
  uint8_t channel_first_;
  uint8_t channel_count_;

  QueueHandle_t queue_ = nullptr;
  FrontlightFader fader_;

  // User-configured brightness (what kOn resumes to). Distinct from
  // the fader's current/target so a flash doesn't clobber it.
  uint16_t configured_brightness_ = frontlight::kDefaultMaxDuty;
  bool logical_on_ = false;

  // Outer cadence (ms) for staggered flash animation; stagger delay
  // per LED = effect_delay_ms_ / channel_count_.
  volatile uint32_t effect_delay_ms_ = frontlight::kDefaultEffectDelayMs;

  // Live runtime gates fed by NVS prefs at boot + by the
  // on_frontlight_changed PATCH hook. Defaults track the v3 firmware
  // (DEFAULT_FL_ALWAYS_ON=true, DEFAULT_DISABLE_FL=false,
  // DEFAULT_FL_FLASH_ON_UPDATE=true) so a fresh install behaves like
  // the Arduino-era device. volatile so the Post() reader sees the
  // latest store from the PATCH thread without a fence (single-bool
  // racing values converge to one of {true,false}).
  volatile bool disabled_ = false;
  volatile bool always_on_ = true;
  volatile bool flash_on_update_ = true;
  // flOffOnDnd default true: existing behaviour was hardcoded "DND
  // suppresses the frontlight" before this gate was added; opting out
  // is a deliberate user choice via /api/settings.
  volatile bool off_on_dnd_ = true;

  // Ambient-light state. `policy_` owns the hysteresis latch + the
  // dark-mode detection; `OnAmbientLux()` is the single point that
  // feeds it. The policy's config is mirrored into the getters so the
  // /api/frontlight/status response keeps working without exposing the
  // policy type through the public surface.
  FrontlightAmbientPolicy policy_;

  // DND predicate. Set once at boot, read from both the caller thread
  // (inside Post) and the controller task. Nullptr = no gating.
  std::function<bool()> suppressor_;

  // Last seen "suppressed" state — `dnd_active && off_on_dnd_`.
  // Drives OnDndStateMaybeChanged()'s edge detection. Initial value
  // false matches boot reality: at startup the panel is dark and DND
  // can't yet have been entered; the first periodic tick catches up
  // (no transient if both flags are false; correct fade-off if DND
  // was already active at boot and `flOffOnDnd` is on).
  volatile bool last_dnd_suppressed_ = false;

  // Per-channel duty mirror — updated alongside every PCA9685 write so
  // GetStatus() can report the actual per-LED state, including during
  // a staggered flash. Cap is the largest variant (V8: 8 channels).
  uint16_t channel_duties_[8] = {0};

  // Manual override staging area — written by SetChannelDuties from the
  // caller thread, drained by the task on a kSetChannelDuties event so
  // the array isn't torn during a partial copy. Mutex is the simplest
  // primitive that doesn't drag a critical-section type into the API.
  std::mutex manual_mu_;
  uint16_t pending_manual_duties_[8] = {0};
  uint8_t pending_manual_count_ = 0;
};

}  // namespace btclock

#endif  // BTCLOCK_HAS_FRONTLIGHT
