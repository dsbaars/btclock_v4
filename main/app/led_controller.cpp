#include "app/led_controller.hpp"

#include <array>
#include <cassert>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace btclock {
namespace {

constexpr const char* kTag = "led";

struct Rgb {
  uint8_t r, g, b;
};
constexpr std::array<Rgb, 6> kBootPalette = {{
    {64, 0, 0}, {64, 32, 0}, {64, 64, 0},
    {0, 64, 0}, {0, 0, 64}, {32, 0, 64},
}};

// Internal state shared between InitLeds() and the task. The queue is
// created before the task starts; PostLedEvent drops silently until
// InitLeds() runs, which is fine for PoC boot sequencing.
QueueHandle_t g_queue = nullptr;
uint32_t g_count = 0;

int64_t MsNow() { return esp_timer_get_time() / 1000; }

void SetAll(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b) {
  for (uint32_t i = 0; i < g_count; ++i) {
    led_strip_set_pixel(strip, i, r, g, b);
  }
  led_strip_refresh(strip);
}

void Task(void* arg) {
  auto* strip = static_cast<led_strip_handle_t>(arg);

  enum class Mode : uint8_t { kBoot, kIdle, kBlockFlash };
  Mode mode = Mode::kBoot;
  int64_t flash_until_ms = 0;
  size_t frame = 0;

  while (true) {
    TickType_t wait;
    switch (mode) {
      case Mode::kBoot:       wait = pdMS_TO_TICKS(250); break;
      case Mode::kBlockFlash: wait = pdMS_TO_TICKS(100); break;
      case Mode::kIdle:       wait = portMAX_DELAY;      break;
      default:                wait = pdMS_TO_TICKS(500); break;
    }

    LedEvent ev;
    if (xQueueReceive(g_queue, &ev, wait) == pdTRUE) {
      switch (ev) {
        case LedEvent::kSetBoot:
          mode = Mode::kBoot;
          frame = 0;
          break;
        case LedEvent::kSetIdle:
          mode = Mode::kIdle;
          break;
        case LedEvent::kBlockFlash:
          mode = Mode::kBlockFlash;
          flash_until_ms = MsNow() + 4000;
          frame = 0;
          break;
      }
    }

    switch (mode) {
      case Mode::kBoot: {
        for (uint32_t i = 0; i < g_count; ++i) {
          const Rgb& c = kBootPalette[(frame + i) % kBootPalette.size()];
          led_strip_set_pixel(strip, i, c.r, c.g, c.b);
        }
        led_strip_refresh(strip);
        ++frame;
        break;
      }
      case Mode::kBlockFlash: {
        if (MsNow() >= flash_until_ms) {
          mode = Mode::kIdle;
          SetAll(strip, 0, 0, 0);
        } else {
          const bool on = (frame & 1) == 0;
          SetAll(strip, on ? 180 : 0, on ? 80 : 0, on ? 10 : 0);
          ++frame;
        }
        break;
      }
      case Mode::kIdle: {
        SetAll(strip, 0, 0, 0);
        break;
      }
    }
  }
}

led_strip_handle_t InitStrip(gpio_num_t pin, uint32_t count) {
  led_strip_config_t strip_cfg = {};
  strip_cfg.strip_gpio_num = pin;
  strip_cfg.max_leds = count;
  strip_cfg.led_model = LED_MODEL_WS2812;
  strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rmt_cfg = {};
  rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_cfg.resolution_hz = 10 * 1000 * 1000;
  rmt_cfg.mem_block_symbols = 64;
  led_strip_handle_t strip = nullptr;
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));
  ESP_ERROR_CHECK(led_strip_clear(strip));
  return strip;
}

}  // namespace

void InitLeds(gpio_num_t pin, uint32_t count) {
  g_count = count;
  g_queue = xQueueCreate(8, sizeof(LedEvent));
  assert(g_queue != nullptr);
  led_strip_handle_t strip = InitStrip(pin, count);
  xTaskCreate(Task, "leds", 4096, strip, tskIDLE_PRIORITY + 1, nullptr);
  ESP_LOGI(kTag, "init: pin=%d count=%u", static_cast<int>(pin),
           static_cast<unsigned>(count));
}

void PostLedEvent(LedEvent ev) {
  if (g_queue != nullptr) xQueueSend(g_queue, &ev, 0);
}

}  // namespace btclock
