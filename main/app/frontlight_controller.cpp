#include "app/frontlight_controller.hpp"

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
      fader_(frontlight::kDefaultMaxDuty, frontlight::kFadeStep) {}

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
  if (queue_ != nullptr) xQueueSend(queue_, &cmd, 0);
}

void FrontlightController::OnAmbientLux(float lux) {
  if (!ambient_enabled_ || lux < 0.0f) return;
  const uint32_t threshold = lux_threshold_;
  if (threshold == 0) return;  // 0 = feature disabled (matches old fw)
  if (static_cast<uint32_t>(lux) < threshold) {
    if (!logical_on_) On();
  } else {
    if (logical_on_) Off();
  }
}

FrontlightController::Status FrontlightController::GetStatus() const {
  Status s{};
  s.enabled = logical_on_;
  s.current_duty = fader_.current();
  s.target_duty = fader_.target();
  s.configured_brightness = configured_brightness_;
  s.lux_threshold = lux_threshold_;
  s.ambient_auto_off = ambient_enabled_;
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
          logical_on_ = true;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(configured_brightness_);
          break;
        case FrontlightEvent::kOff:
          logical_on_ = false;
          pulse = PulsePhase::kIdle;
          fader_.SetTarget(0);
          break;
        case FrontlightEvent::kSetBrightness:
          configured_brightness_ = cmd.value;
          // Only move the fader if the light is logically on; an off
          // light stays dark but remembers the new level for next kOn.
          if (logical_on_) {
            pulse = PulsePhase::kIdle;
            fader_.SetTarget(configured_brightness_);
          }
          break;
        case FrontlightEvent::kBlockFlash:
        case FrontlightEvent::kZapFlash:
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
