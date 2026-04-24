#include "io/frontlight_controller.hpp"

#include <cassert>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace btclock {
namespace {

constexpr const char* kTag = "frontlight";

int64_t MsNow() { return esp_timer_get_time() / 1000; }

}  // namespace

// ----------------------------------------------------------- controller

FrontlightController::FrontlightController(Pca9685& pca, uint8_t channel_first,
                                            uint8_t channel_count)
    : pca_(pca),
      channel_first_(channel_first),
      channel_count_(channel_count),
      fader_(frontlight::kDefaultMaxDuty, frontlight::kFadeStep) {
  // Seed the policy with the same defaults the old header exposed as
  // constants. `init_hardware` overwrites these at boot from NVS, but
  // if that call never happens (unit test fixture, hardware init stub)
  // the controller still behaves like v3's defaults.
  FrontlightAmbientConfig cfg{};
  cfg.ambient_auto_enabled = true;
  cfg.lux_threshold = frontlight::kDefaultLuxThreshold;
  cfg.off_when_dark = false;
  policy_.SetConfig(cfg);
}

void FrontlightController::Start() {
  // Queue depth 8: matches led_controller; deeper buffering is
  // unnecessary because events are coarse (on/off/flash) and duplicates
  // collapse harmlessly in the fader's target.
  queue_ = xQueueCreate(8, sizeof(FrontlightCommand));
  assert(queue_ != nullptr);
  // Boot state: outputs disabled, fader at 0. kOn from main.cpp ramps
  // us up to the configured brightness.
  WriteAllChannels(0);
  xTaskCreate(&FrontlightController::TaskTrampoline, "frontlight", 3072,
              this, tskIDLE_PRIORITY + 1, nullptr);
  ESP_LOGI(kTag, "init: ch=[%u..%u] max_duty=%u",
           static_cast<unsigned>(channel_first_),
           static_cast<unsigned>(channel_first_ + channel_count_ - 1),
           static_cast<unsigned>(configured_brightness_));
}

void FrontlightController::Post(FrontlightCommand cmd) {
  if (queue_ == nullptr) return;
  // DND gate — matches the LedHandler::frontlight* short-circuits in
  // the old firmware. Anything that would light the backlight is
  // dropped, and any in-flight On/SetBrightness is cancelled by a
  // forced kOff so a DND window armed while the light is already on
  // fades the panel to black without the caller having to know.
  // kAmbientOn / kAmbientOff are treated like kOn / kOff here so DND
  // wins over the ambient loop too.
  if (suppressor_ && suppressor_()) {
    // Forward a user-off verbatim so the latch still tracks user
    // intent even inside a DND window. Everything else becomes a
    // kAmbientOff — we fade to black (or stay black) without setting
    // the user-off latch, so when DND lifts the ambient loop can
    // resume control on the next lux sample.
    if (cmd.event == FrontlightEvent::kOff) {
      xQueueSend(queue_, &cmd, 0);
    } else {
      const FrontlightCommand off{FrontlightEvent::kAmbientOff, 0};
      xQueueSend(queue_, &off, 0);
    }
    return;
  }
  xQueueSend(queue_, &cmd, 0);
}

// --- Runtime config forwarders ---------------------------------------

void FrontlightController::SetAmbientAutoOff(bool enabled) {
  FrontlightAmbientConfig cfg = policy_.config();
  cfg.ambient_auto_enabled = enabled;
  policy_.SetConfig(cfg);
}

bool FrontlightController::ambient_auto_off() const {
  return policy_.config().ambient_auto_enabled;
}

void FrontlightController::SetLuxThreshold(uint32_t lux) {
  FrontlightAmbientConfig cfg = policy_.config();
  cfg.lux_threshold = lux;
  policy_.SetConfig(cfg);
}

uint32_t FrontlightController::lux_threshold() const {
  return policy_.config().lux_threshold;
}

void FrontlightController::SetOffWhenDark(bool enabled) {
  FrontlightAmbientConfig cfg = policy_.config();
  cfg.off_when_dark = enabled;
  policy_.SetConfig(cfg);
}

bool FrontlightController::off_when_dark() const {
  return policy_.config().off_when_dark;
}

void FrontlightController::SetConfiguredBrightness(uint16_t duty) {
  // Keep configured_brightness_ in lock-step with the queue so a later
  // kOn resumes at the right level even if the fader isn't running yet.
  // The task's kSetBrightness branch re-assigns under the task's own
  // ordering so the value is eventually consistent.
  SetBrightness(duty);
}

void FrontlightController::OnAmbientLux(float lux) {
  // The policy reads user_off_ + output_on_ + hysteresis state to decide
  // what event to post. Keeps the off-when-dark + user-off + regular
  // threshold interactions in one pure-logic spot, covered by host
  // tests. The controller task reconciles logical_on_ from the event
  // it receives, which keeps the user-off latch single-sourced.
  if (logical_on_) {
    policy_.NoteOutputOn();
  } else {
    policy_.NoteOutputOff();
  }

  const auto action = policy_.Evaluate(lux);
  switch (action) {
    case FrontlightAmbientAction::kNone:
      return;
    case FrontlightAmbientAction::kOn:
      Post({FrontlightEvent::kAmbientOn, 0});
      return;
    case FrontlightAmbientAction::kOff:
      Post({FrontlightEvent::kAmbientOff, 0});
      return;
  }
}

FrontlightController::Status FrontlightController::GetStatus() const {
  Status s{};
  s.enabled = logical_on_;
  s.current_duty = fader_.current();
  s.target_duty = fader_.target();
  s.configured_brightness = configured_brightness_;
  const auto& cfg = policy_.config();
  s.lux_threshold = cfg.lux_threshold;
  s.ambient_auto_off = cfg.ambient_auto_enabled;
  return s;
}

void FrontlightController::WriteAllChannels(uint16_t duty) {
  for (uint8_t i = 0; i < channel_count_; ++i) {
    pca_.SetDuty(static_cast<uint8_t>(channel_first_ + i), duty);
  }
}

void FrontlightController::TaskTrampoline(void* arg) {
  static_cast<FrontlightController*>(arg)->TaskLoop();
}

void FrontlightController::TaskLoop() {
  // Pulse state machine: on a kBlockFlash/kZapFlash the controller
  // captures the pre-flash target, ramps up to max, holds, then
  // restores. A mid-pulse event (e.g. Off while flashing) wins: the
  // pulse aborts and the new target takes over.
  enum class PulsePhase : uint8_t { kIdle, kRampUp, kHold, kRampDown };
  PulsePhase pulse = PulsePhase::kIdle;
  int64_t hold_until_ms = 0;
  uint32_t hold_ms = 0;
  uint16_t pre_pulse_target = 0;

  while (true) {
    // Block until there's work, unless we're mid-transition — then
    // wake every kTickMs to step the fader.
    const bool animating =
        !fader_.AtTarget() || pulse != PulsePhase::kIdle;
    const TickType_t wait =
        animating ? pdMS_TO_TICKS(frontlight::kTickMs) : portMAX_DELAY;

    FrontlightCommand cmd{};
    if (xQueueReceive(queue_, &cmd, wait) == pdTRUE) {
      switch (cmd.event) {
        case FrontlightEvent::kOn:
          // User intent: light on. Clears the user-off latch so the
          // ambient loop is free to act again on the next sample.
          policy_.SetUserOff(false);
          logical_on_ = true;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(configured_brightness_);
          break;
        case FrontlightEvent::kOff:
          // User intent: light off. Latch it. The ambient loop will
          // keep hands off until kOn / kSetBrightness / a flash clears
          // the latch again.
          policy_.SetUserOff(true);
          logical_on_ = false;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(0);
          break;
        case FrontlightEvent::kAmbientOn:
          // Ambient-driven on: never overrides a user-off latch. The
          // policy already gates this at source; the second check here
          // is defence against a stale queue entry from before a kOff.
          if (policy_.user_off()) break;
          logical_on_ = true;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(configured_brightness_);
          break;
        case FrontlightEvent::kAmbientOff:
          // Ambient-driven off: fades to 0 without setting the latch,
          // so a future lux rise can auto-on it again.
          logical_on_ = false;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(0);
          break;
        case FrontlightEvent::kSetBrightness:
          configured_brightness_ = cmd.value;
          // Only move the fader if the light is logically on; an off
          // light stays dark but remembers the new level for next kOn.
          // A non-zero brightness write also clears the user-off latch
          // — the user is explicitly asking for "this much light", so
          // keeping them dark would be perverse.
          if (cmd.value > 0) {
            policy_.SetUserOff(false);
          }
          if (logical_on_) {
            pulse = PulsePhase::kIdle;
            fader_.SetTarget(configured_brightness_);
          }
          break;
        case FrontlightEvent::kBlockFlash:
        case FrontlightEvent::kZapFlash:
          // Flash events count as explicit user-visible acknowledgements,
          // so they clear the user-off latch (matches the task brief:
          // "until something explicit turns it back on — button press,
          // next zap, next block flash, or POST /api/frontlight/on").
          // The pulse itself still returns to the pre-flash state
          // (dark if we were dark, bright if we were bright).
          policy_.SetUserOff(false);
          hold_ms = (cmd.event == FrontlightEvent::kBlockFlash)
                        ? frontlight::kBlockFlashHoldMs
                        : frontlight::kZapFlashHoldMs;
          pre_pulse_target = logical_on_ ? configured_brightness_ : 0;
          pulse = PulsePhase::kRampUp;
          fader_.SetTarget(frontlight::kDefaultMaxDuty);
          break;
      }
    }

    // Advance the fader + pulse state machine.
    const uint16_t duty = fader_.Step();
    WriteAllChannels(duty);

    switch (pulse) {
      case PulsePhase::kIdle:
        break;
      case PulsePhase::kRampUp:
        if (fader_.AtTarget()) {
          pulse = PulsePhase::kHold;
          hold_until_ms = MsNow() + static_cast<int64_t>(hold_ms);
        }
        break;
      case PulsePhase::kHold:
        if (MsNow() >= hold_until_ms) {
          pulse = PulsePhase::kRampDown;
          fader_.SetTarget(pre_pulse_target);
        }
        break;
      case PulsePhase::kRampDown:
        if (fader_.AtTarget()) pulse = PulsePhase::kIdle;
        break;
    }
  }
}

}  // namespace btclock
