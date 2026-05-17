// Whole TU is variant-gated: on boards without a PCA9685 backlight the
// FrontlightController class doesn't exist (the header is also gated on
// BTCLOCK_HAS_FRONTLIGHT), so compiling this .cpp would fail. Wrapping
// the whole body keeps the file in main/CMakeLists.txt SRCS unchanged.
#if BTCLOCK_HAS_FRONTLIGHT

#include "io/frontlight_controller.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "queue_metrics.hpp"

namespace btclock {
namespace {

constexpr const char* kTag = "frontlight";

int64_t MsNow() {
  return esp_timer_get_time() / 1000;
}

// Send-or-record helper. Every queue send below is non-blocking (we'd
// rather drop than stall the caller — see Post()'s WHY); a wedged
// task is then visible via /api/system_status's queueDrops field.
void SendOrDrop(QueueHandle_t q, const FrontlightCommand& cmd) {
  if (xQueueSend(q, &cmd, 0) != pdTRUE) {
    queue_metrics::RecordDrop(queue_metrics::Queue::kFrontlight);
  }
}

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
  //
  // On heap exhaustion either step can fail. Production sdkconfig has
  // assertions silenced, so we must check explicitly: if the queue or
  // the task can't be created, leave queue_ as nullptr — Post() already
  // short-circuits on that, so the controller degrades to a no-op
  // instead of spawning a task that would xQueueReceive(nullptr).
  queue_ = xQueueCreate(8, sizeof(FrontlightCommand));
  if (queue_ == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate failed; frontlight disabled");
    return;
  }
  // Boot state: outputs disabled, fader at 0. kOn from main.cpp ramps
  // us up to the configured brightness.
  WriteAllChannels(0);
  if (xTaskCreate(&FrontlightController::TaskTrampoline, "frontlight", 3072,
                  this, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "xTaskCreate failed; frontlight disabled");
    vQueueDelete(queue_);
    queue_ = nullptr;
    return;
  }
  ESP_LOGI(kTag, "init: ch=[%u..%u] max_duty=%u",
           static_cast<unsigned>(channel_first_),
           static_cast<unsigned>(channel_first_ + channel_count_ - 1),
           static_cast<unsigned>(configured_brightness_));
}

void FrontlightController::Post(FrontlightCommand cmd) {
  if (queue_ == nullptr) return;
  // flDisable wins over everything: it's the user's hard mute. We
  // must come before flAlwaysOn (which would otherwise prevent the
  // forced fade-out) and before the DND gate (DND-or-disabled is
  // still off). kOff is forwarded verbatim so the user-off latch
  // still tracks intent; anything else becomes kAmbientOff so when
  // disable lifts the ambient loop can resume on the next sample.
  if (disabled_) {
    if (cmd.event == FrontlightEvent::kOff) {
      SendOrDrop(queue_, cmd);
    } else {
      const FrontlightCommand off{FrontlightEvent::kAmbientOff, 0};
      SendOrDrop(queue_, off);
    }
    return;
  }
  // flAlwaysOn — drop the regular ambient-off event so the BH1750
  // loop's high-lux branch can't fade the panel out. kDarkOff is
  // intentionally NOT gated here: off-when-dark is a more specific
  // user override and wins over always_on in pitch black. User-
  // initiated kOff still goes through (the user-off latch remains
  // authoritative).
  if (always_on_ && cmd.event == FrontlightEvent::kAmbientOff) {
    return;
  }
  // flFlashOnUpd — gate the block-flash pulse. Off means "user does
  // not want the backlight to react to data updates". Zap flash is
  // a separate pref (flFlashOnZap) and stays unaffected.
  if (cmd.event == FrontlightEvent::kBlockFlash && !flash_on_update_) {
    return;
  }
  // DND gate — matches the LedHandler::frontlight* short-circuits in
  // the old firmware. Anything that would light the backlight is
  // dropped, and any in-flight On/SetBrightness is cancelled by a
  // forced kOff so a DND window armed while the light is already on
  // fades the panel to black without the caller having to know.
  // kAmbientOn / kAmbientOff are treated like kOn / kOff here so DND
  // wins over the ambient loop too. `flOffOnDnd` (off_on_dnd_) is the
  // user opt-out — when false, an active DND window no longer mutes
  // the frontlight and the LED ring's DND gate (LedController) stays
  // the sole muting path.
  if (off_on_dnd_ && suppressor_ && suppressor_()) {
    // Forward a user-off verbatim so the latch still tracks user
    // intent even inside a DND window. Everything else becomes a
    // kAmbientOff — we fade to black (or stay black) without setting
    // the user-off latch, so when DND lifts the ambient loop can
    // resume control on the next lux sample.
    if (cmd.event == FrontlightEvent::kOff) {
      SendOrDrop(queue_, cmd);
    } else {
      const FrontlightCommand off{FrontlightEvent::kAmbientOff, 0};
      SendOrDrop(queue_, off);
    }
    return;
  }
  SendOrDrop(queue_, cmd);
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
      // Distinguish dark-driven off from threshold-driven off so the
      // flAlwaysOn gate in Post() only swallows the latter. After
      // Evaluate(), is_dark() is true iff the kOff came from the
      // off-when-dark branch (the policy sets dark_ before returning).
      Post({policy_.is_dark() ? FrontlightEvent::kDarkOff
                              : FrontlightEvent::kAmbientOff,
            0});
      return;
  }
}

void FrontlightController::SetChannelDuties(const uint16_t* duties,
                                            uint8_t count) {
  if (queue_ == nullptr || duties == nullptr) return;
  const uint8_t cap =
      sizeof(pending_manual_duties_) / sizeof(pending_manual_duties_[0]);
  const uint8_t clamped = (count < cap) ? count : cap;
  {
    std::lock_guard<std::mutex> lk(manual_mu_);
    pending_manual_count_ = clamped;
    for (uint8_t i = 0; i < clamped; ++i) {
      pending_manual_duties_[i] = duties[i];
    }
  }
  const FrontlightCommand cmd{FrontlightEvent::kSetChannelDuties, 0};
  SendOrDrop(queue_, cmd);
}

void FrontlightController::OnDndStateMaybeChanged() {
  // Compute the effective suppression bit: DND is active AND the user
  // hasn't opted out via flOffOnDnd. Edge-triggered so we only push an
  // event when the suppression bit actually flips. Cheap on the steady
  // path: a function-call + std::time() through dnd::IsActive().
  if (queue_ == nullptr) return;
  const bool dnd_active = (suppressor_ && suppressor_());
  const bool currently_suppressed = dnd_active && off_on_dnd_;
  const bool was_suppressed = last_dnd_suppressed_;
  last_dnd_suppressed_ = currently_suppressed;
  if (currently_suppressed == was_suppressed) return;
  // Bypass Post()'s always_on gate by enqueueing directly: when DND is
  // active we want the panel dark, period — flAlwaysOn is a secondary
  // ambient-loop preference and DND is a higher-priority user policy.
  // (Routing through Post() would silently drop kAmbientOff because of
  // the always_on check that lives above the suppressor branch.) For
  // the DND-lifts edge we still send kAmbientOn directly so it is
  // symmetric and not subject to anything else; the task gates on the
  // user-off latch internally so a previous explicit Off is honoured.
  const FrontlightCommand cmd =
      currently_suppressed ? FrontlightCommand{FrontlightEvent::kAmbientOff, 0}
                           : FrontlightCommand{FrontlightEvent::kAmbientOn, 0};
  SendOrDrop(queue_, cmd);
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
  s.channel_count = channel_count_;
  const uint8_t cap = sizeof(s.duties) / sizeof(s.duties[0]);
  const uint8_t n = channel_count_ < cap ? channel_count_ : cap;
  for (uint8_t i = 0; i < n; ++i) s.duties[i] = channel_duties_[i];
  return s;
}

void FrontlightController::WriteAllChannels(uint16_t duty) {
  const uint8_t cap = sizeof(channel_duties_) / sizeof(channel_duties_[0]);
  for (uint8_t i = 0; i < channel_count_; ++i) {
    pca_.SetDuty(static_cast<uint8_t>(channel_first_ + i), duty);
    if (i < cap) channel_duties_[i] = duty;
  }
}

void FrontlightController::WriteStaggeredTick(uint32_t tick,
                                              uint16_t max_brightness,
                                              StaggerDirection direction) {
  // Per-LED duty uses the pure helper so host tests exercise the same
  // math the hardware sees. `i` here is the stagger index 0..N-1; it
  // maps to PCA channel `channel_first_ + i`. v3 pinned index 0 as
  // the first-to-light LED; we preserve that so the cascade direction
  // across the physical panel matches v3.
  const uint8_t cap = sizeof(channel_duties_) / sizeof(channel_duties_[0]);
  for (uint8_t i = 0; i < channel_count_; ++i) {
    const uint16_t duty =
        ComputeStaggeredDuty(tick, i, channel_count_, max_brightness,
                             frontlight::kFadeStep, direction);
    pca_.SetDuty(static_cast<uint8_t>(channel_first_ + i), duty);
    if (i < cap) channel_duties_[i] = duty;
  }
}

void FrontlightController::TaskTrampoline(void* arg) {
  static_cast<FrontlightController*>(arg)->TaskLoop();
}

void FrontlightController::TaskLoop() {
  // Pulse state machine — two-phase staggered animation, mirroring v3
  // `LedHandler::frontlightFlash` (the v3 firmware's
  // src/lib/drivers/leds/led_handler.cpp:515-527). Direction sequence:
  //
  //   if pre-flash was on : kOut  (dim cascade) -> hold -> kIn  (restore)
  //   if pre-flash was off: kIn   (lit cascade) -> hold -> kOut (restore)
  //
  // While kOut / kIn runs the fader is paused and the task drives the
  // PCA9685 channels directly via WriteStaggeredTick(). That bypass is
  // necessary because the staggered animation writes different duties
  // per channel at the same tick — the single-duty fader model cannot
  // express that. After the second half of the stagger ends we snap
  // the fader to the pre-flash target so subsequent kOn / kOff events
  // interpolate from the correct current value.
  //
  // A mid-pulse kOn / kOff / kSetBrightness / kAmbient* wins: the
  // stagger is cancelled (pulse -> kIdle) and the new intent routes
  // through the fader on the next tick.
  enum class PulsePhase : uint8_t {
    kIdle,
    kFirstHalf,   // cascade from the pre-flash state to its inverse
    kMidHold,     // brief hold at the inverted state (block=0ms, zap small)
    kSecondHalf,  // cascade back to the pre-flash state
  };
  PulsePhase pulse = PulsePhase::kIdle;

  // Per-pulse captured state (snapshot at kBlockFlash/kZapFlash
  // receipt so a concurrent PATCH doesn't mutate mid-animation).
  uint16_t pulse_max_brightness = frontlight::kDefaultMaxDuty;
  uint32_t pulse_total_ticks = 0;
  uint32_t pulse_tick_delay_ms = frontlight::kTickMs;
  uint32_t pulse_mid_hold_ms = 0;
  uint32_t pulse_tick = 0;
  int64_t pulse_hold_until_ms = 0;
  // first_half_in tracks which direction the first half uses. Inverse
  // drives the second half. Matches v3: if pre-flash bright, first is
  // kOut (dim cascade); if pre-flash dark, first is kIn (lit cascade).
  bool first_half_in = true;
  StaggerDirection first_dir = StaggerDirection::kIn;

  auto start_pulse = [&](FrontlightEvent ev) {
    // Flash events count as explicit user-visible acknowledgements,
    // so they clear the user-off latch — matches v3 semantics and the
    // task brief: a block flash should re-enable the ambient loop.
    policy_.SetUserOff(false);

    // Snapshot tunables so a concurrent PATCH can't change them mid-
    // pulse. `effect_delay_ms_` is volatile for the read.
    pulse_max_brightness = configured_brightness_ > 0
                               ? configured_brightness_
                               : frontlight::kDefaultMaxDuty;
    const uint32_t effect_delay = effect_delay_ms_;
    pulse_total_ticks = StaggerTotalTicks(channel_count_, pulse_max_brightness,
                                          frontlight::kFadeStep);
    pulse_tick_delay_ms = StaggerDelayMs(effect_delay, channel_count_);
    pulse_mid_hold_ms = (ev == FrontlightEvent::kBlockFlash)
                            ? frontlight::kBlockFlashHoldMs
                            : frontlight::kZapFlashHoldMs;

    // Pre-flash state picks direction. v3: frontlightOn ? out+in : in+out.
    first_half_in = !logical_on_;
    first_dir = first_half_in ? StaggerDirection::kIn : StaggerDirection::kOut;

    pulse_tick = 0;
    pulse = PulsePhase::kFirstHalf;
    // Pause the fader so its Step() doesn't fight our per-channel
    // writes. Snap to the current visible duty on first_dir's tick 0.
    fader_.Snap(first_dir == StaggerDirection::kIn ? 0 : pulse_max_brightness);
  };

  auto cancel_pulse = [&]() {
    pulse = PulsePhase::kIdle;
    pulse_tick = 0;
  };

  while (true) {
    // Choose the wait period. During a stagger phase we wake at the
    // per-LED cadence so the cascade rate is driven by flEffectDelay.
    // During fader transitions we wake every kTickMs. Otherwise block.
    TickType_t wait;
    if (pulse == PulsePhase::kFirstHalf || pulse == PulsePhase::kSecondHalf) {
      wait = pdMS_TO_TICKS(pulse_tick_delay_ms);
    } else if (pulse == PulsePhase::kMidHold) {
      wait = pdMS_TO_TICKS(10);  // tight poll for the hold deadline
    } else if (!fader_.AtTarget()) {
      wait = pdMS_TO_TICKS(frontlight::kTickMs);
    } else {
      wait = portMAX_DELAY;
    }

    FrontlightCommand cmd{};
    if (xQueueReceive(queue_, &cmd, wait) == pdTRUE) {
      switch (cmd.event) {
        case FrontlightEvent::kOn:
          policy_.SetUserOff(false);
          logical_on_ = true;
          cancel_pulse();
          fader_.SetTarget(configured_brightness_);
          break;
        case FrontlightEvent::kOff:
          policy_.SetUserOff(true);
          logical_on_ = false;
          cancel_pulse();
          fader_.SetTarget(0);
          break;
        case FrontlightEvent::kAmbientOn:
          if (policy_.user_off()) break;
          logical_on_ = true;
          cancel_pulse();
          fader_.SetTarget(configured_brightness_);
          break;
        case FrontlightEvent::kAmbientOff:
        case FrontlightEvent::kDarkOff:
          logical_on_ = false;
          cancel_pulse();
          fader_.SetTarget(0);
          break;
        case FrontlightEvent::kSetBrightness:
          configured_brightness_ = cmd.value;
          if (cmd.value > 0) {
            policy_.SetUserOff(false);
          }
          if (logical_on_) {
            cancel_pulse();
            fader_.SetTarget(configured_brightness_);
          }
          break;
        case FrontlightEvent::kSetChannelDuties: {
          // Manual override — drain the staged duties under the mutex
          // so a concurrent SetChannelDuties caller can't tear the read.
          uint16_t local[8] = {0};
          uint8_t n = 0;
          {
            std::lock_guard<std::mutex> lk(manual_mu_);
            n = pending_manual_count_;
            for (uint8_t i = 0; i < n; ++i)
              local[i] = pending_manual_duties_[i];
          }
          cancel_pulse();
          // Bypass WriteAllChannels because the duties are per-channel.
          // Also clear the user-off latch: a non-zero manual write is
          // an explicit user intent to light something, so the next
          // ambient cycle shouldn't suppress us. logical_on_ tracks
          // "any channel non-zero" so /api/status reflects reality.
          bool any_on = false;
          const uint8_t cap =
              sizeof(channel_duties_) / sizeof(channel_duties_[0]);
          for (uint8_t i = 0; i < channel_count_ && i < cap && i < n; ++i) {
            pca_.SetDuty(static_cast<uint8_t>(channel_first_ + i), local[i]);
            channel_duties_[i] = local[i];
            if (local[i] > 0) any_on = true;
          }
          // Snap the fader so kIdle's Step()+WriteAllChannels can't
          // re-paint a uniform duty over our zebra. Use the first
          // channel's value as the "global" approximation; it's the
          // closest single number we can give the fader.
          fader_.Snap(n > 0 ? local[0] : 0);
          logical_on_ = any_on;
          if (any_on) policy_.SetUserOff(false);
          break;
        }
        case FrontlightEvent::kBlockFlash:
        case FrontlightEvent::kZapFlash:
          start_pulse(cmd.event);
          // Emit tick 0 immediately so the user sees the first phase
          // of the cascade on the same loop iteration the event arrived.
          WriteStaggeredTick(pulse_tick, pulse_max_brightness, first_dir);
          pulse_tick = 1;
          break;
      }
      // Skip the per-tick advance on an event-receive cycle — next
      // iteration will advance us at the correct wait cadence.
      continue;
    }

    // No event: advance whichever animation is active.
    switch (pulse) {
      case PulsePhase::kIdle: {
        // Normal fade state machine.
        const uint16_t duty = fader_.Step();
        WriteAllChannels(duty);
        break;
      }
      case PulsePhase::kFirstHalf: {
        if (pulse_tick < pulse_total_ticks) {
          WriteStaggeredTick(pulse_tick, pulse_max_brightness, first_dir);
          ++pulse_tick;
        } else {
          // First half done — every LED landed on the inverted state.
          // Enter mid-hold (may be 0 for block flash, in which case we
          // immediately fall through to kSecondHalf on the next iter).
          pulse_hold_until_ms =
              MsNow() + static_cast<int64_t>(pulse_mid_hold_ms);
          pulse = PulsePhase::kMidHold;
          pulse_tick = 0;
        }
        break;
      }
      case PulsePhase::kMidHold: {
        if (MsNow() >= pulse_hold_until_ms) {
          pulse = PulsePhase::kSecondHalf;
          pulse_tick = 0;
        }
        break;
      }
      case PulsePhase::kSecondHalf: {
        // Inverse direction of the first half.
        const StaggerDirection second_dir = first_dir == StaggerDirection::kIn
                                                ? StaggerDirection::kOut
                                                : StaggerDirection::kIn;
        if (pulse_tick < pulse_total_ticks) {
          WriteStaggeredTick(pulse_tick, pulse_max_brightness, second_dir);
          ++pulse_tick;
        } else {
          // Stagger complete. Restore the fader to the pre-flash state
          // so subsequent kOn/kOff interpolate from the right point.
          // v3 left whatever state the last fade ended on — which is
          // exactly what `configured_brightness_` (if on) or 0 (if off)
          // represents here.
          const uint16_t restore = logical_on_ ? configured_brightness_ : 0;
          fader_.Snap(restore);
          WriteAllChannels(restore);
          pulse = PulsePhase::kIdle;
          pulse_tick = 0;
        }
        break;
      }
    }
  }
}

}  // namespace btclock

#endif  // BTCLOCK_HAS_FRONTLIGHT
