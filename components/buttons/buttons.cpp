#include "buttons.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace btclock {
namespace {

constexpr const char* kTag = "buttons";

// Polling / timing parameters. All values are expressed in polling
// ticks (one tick == kPollPeriodMs) except where noted.
constexpr uint32_t kPollPeriodMs = 20;
constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kLongPressMs = 800;
// 5 s hold of all four buttons fires the factory-reset combo.
// Intentionally much longer than kLongPressMs so a user who rests a
// hand across the bezel can't trigger a wipe by accident.
constexpr uint32_t kAllLongPressMs = 5000;

// 40 ms / 20 ms = 2 consecutive identical samples to accept a change.
constexpr uint8_t kDebounceTicks = kDebounceMs / kPollPeriodMs;
// 800 ms / 20 ms = 40 ticks of continuous press to trigger long-press.
constexpr uint16_t kLongPressTicks = kLongPressMs / kPollPeriodMs;
// 5000 ms / 20 ms = 250 ticks for the all-buttons combo.
constexpr uint32_t kAllLongPressTicks = kAllLongPressMs / kPollPeriodMs;

// Bit mask for pins 0..3 on the MCP23017 port word.
constexpr uint16_t kButtonMask = 0x000F;

}  // namespace

ButtonReader::ButtonReader(Mcp23017& mcp, QueueHandle_t queue)
    : mcp_(mcp), queue_(queue) {}

ButtonReader::~ButtonReader() {
  stop_ = true;
  if (task_ != nullptr) {
    // Wait a couple of poll periods for the task to observe stop_ and
    // exit on its own; then it self-deletes via vTaskDelete(nullptr).
    vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs * 3));
    task_ = nullptr;
  }
}

esp_err_t ButtonReader::Start() {
  if (queue_ == nullptr) {
    ESP_LOGE(kTag, "Start(): null queue");
    return ESP_ERR_INVALID_ARG;
  }
  if (task_ != nullptr) {
    ESP_LOGW(kTag, "Start(): already started");
    return ESP_ERR_INVALID_STATE;
  }
  // 3 KB stack — we do no printf-in-hot-path and the work is trivial.
  if (xTaskCreate(TaskTrampoline, "btn", 3072, this, tskIDLE_PRIORITY + 1,
                  &task_) != pdPASS) {
    ESP_LOGE(kTag, "xTaskCreate failed");
    task_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag, "button reader task started");
  return ESP_OK;
}

void ButtonReader::TaskTrampoline(void* arg) {
  static_cast<ButtonReader*>(arg)->Run();
  vTaskDelete(nullptr);
}

void ButtonReader::Run() {
  const TickType_t period = pdMS_TO_TICKS(kPollPeriodMs);
  TickType_t last_wake = xTaskGetTickCount();

  while (!stop_) {
    vTaskDelayUntil(&last_wake, period);

    uint16_t raw_port = 0;
    if (mcp_.ReadPort(&raw_port) != ESP_OK) {
      // Transient I2C hiccup: skip this sample rather than forcing a
      // spurious edge. Debounce counters stay put.
      continue;
    }

    for (uint8_t i = 0; i < kNumButtons; ++i) {
      State& s = state_[i];
      // Active-low wiring: a '1' in the port word means the pin is
      // HIGH, i.e. released.
      const bool raw_high = (raw_port & (1u << i)) != 0;

      if (raw_high == s.last_raw) {
        // Same level as last sample — count toward debounce stability.
        if (s.stable_ticks < kDebounceTicks) {
          ++s.stable_ticks;
        }
      } else {
        // Bounce: reset and restart the stability counter at 1 for the
        // new level.
        s.last_raw = raw_high;
        s.stable_ticks = 1;
      }

      // Accept a logical state transition only once the new raw level
      // has held stable for the full debounce window.
      const bool debounced_pressed = !s.last_raw;
      if (s.stable_ticks >= kDebounceTicks && debounced_pressed != s.pressed) {
        if (debounced_pressed) {
          // Rising edge of a press (logical).
          s.pressed = true;
          s.press_ticks = 0;
          s.long_fired = false;
        } else {
          // Falling edge of a press (logical release).
          s.pressed = false;
          // Emit kClick only if we didn't already fire a long-press for
          // this hold. Release-after-long-press is swallowed.
          if (!s.long_fired) {
            const uint8_t logical = inverted_ ? (kNumButtons - 1 - i) : i;
            const ButtonInput ev{static_cast<ButtonId>(logical),
                                 ButtonEvent::kClick};
            // Non-blocking: we'd rather drop an event than stall the
            // poll loop and push all other buttons off-schedule.
            (void)xQueueSend(queue_, &ev, 0);
          }
          s.press_ticks = 0;
        }
      }

      // While debounced-pressed, count elapsed ticks and fire the
      // long-press event exactly at the 800 ms mark. The state stays
      // .pressed until release, and long_fired gates both re-firing
      // and the trailing click.
      if (s.pressed) {
        if (s.press_ticks < UINT16_MAX) ++s.press_ticks;
        if (!s.long_fired && s.press_ticks >= kLongPressTicks) {
          s.long_fired = true;
          const uint8_t logical = inverted_ ? (kNumButtons - 1 - i) : i;
          const ButtonInput ev{static_cast<ButtonId>(logical),
                               ButtonEvent::kLongPress};
          (void)xQueueSend(queue_, &ev, 0);
        }
      }
    }

    // All-buttons-held factory-reset detector. Runs on the debounced
    // .pressed states so a bouncy contact on one button can't stall
    // the combo timer. Counter advances only while all four are held
    // and resets the instant any button is released, matching a user's
    // expectation that "let go" cancels the combo.
    bool all_held = true;
    for (uint8_t i = 0; i < kNumButtons; ++i) {
      if (!state_[i].pressed) {
        all_held = false;
        break;
      }
    }
    if (all_held) {
      if (all_pressed_ticks_ < UINT32_MAX) ++all_pressed_ticks_;
      if (!all_long_fired_ && all_pressed_ticks_ >= kAllLongPressTicks) {
        all_long_fired_ = true;
        if (all_long_press_cb_) {
          ESP_LOGW(kTag, "all-buttons long-press: factory reset combo");
          all_long_press_cb_();
        }
      }
    } else {
      all_pressed_ticks_ = 0;
      all_long_fired_ = false;
    }
  }
}

}  // namespace btclock
