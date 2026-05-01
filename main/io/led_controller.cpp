#include "io/led_controller.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io/led_curves.hpp"
#include "io/led_prefs.hpp"
#include "led_strip.h"
#include "prefs.hpp"
#include "queue_metrics.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {

constexpr const char* kTag = "led";

// LED prefs live in the shared `settings` NVS namespace alongside every
// other PATCH-accepted field, keyed by the same strings the WebUI uses
// (kLedBrightness, kBlockFlashColor, kDisableLeds, kLedFlashOnUpd). An
// earlier port stored them in a separate `led` namespace with hand-
// abbreviated keys; ResolveLedPrefs() handles the one-shot migration so
// installs that pre-date this change keep their saved values.
constexpr const char* kLegacyNvsNamespace = "led";

// Rainbow palette used by kSetBoot + kPowerTest.
struct Rgb {
  uint8_t r, g, b;
};
constexpr std::array<Rgb, 6> kBootPalette = {{
    {64, 0, 0},
    {64, 32, 0},
    {64, 64, 0},
    {0, 64, 0},
    {0, 0, 64},
    {32, 0, 64},
}};

// --- Shared state ---------------------------------------------------

QueueHandle_t g_queue = nullptr;
uint32_t g_count = 0;
// Direct handle to the strip, cached at InitLeds time so the OTA
// progress paint path can bypass the queue + task. The task holds the
// same handle but it's owned by led_strip internally; the driver API
// is safe to call from multiple tasks as long as refreshes don't
// interleave (we serialise via g_direct_mu below).
led_strip_handle_t g_strip = nullptr;
// Serialises direct paint calls so the OTA progress indicator doesn't
// race the task's per-frame led_strip_refresh.
std::mutex g_direct_mu;

// Mutex protects g_state across the LED task and caller threads (HTTP
// worker, data-source callbacks). Coarse lock — good enough given the
// update rate.
std::mutex g_state_mu;
LedState g_state;  // prefs + pixel mirror

// The last "user-set" colour, restored when an effect finishes. Old
// firmware mirrored the pixel array directly; we keep a separate copy
// so effects can freely trample the strip during playback.
std::array<uint32_t, 8> g_resting_pixels = {0};

// DND predicate — when set and true, effect posts drop and the task
// paints black on the next idle cycle. Guarded by g_state_mu so the
// HTTP task can install it without racing the LED task.
std::function<bool()> g_suppressor;

// Runtime helpers ---------------------------------------------------

int64_t MsNow() {
  return esp_timer_get_time() / 1000;
}

uint32_t PackRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

void Unpack(uint32_t rgb, uint8_t* r, uint8_t* g, uint8_t* b) {
  *r = static_cast<uint8_t>((rgb >> 16) & 0xFFu);
  *g = static_cast<uint8_t>((rgb >> 8) & 0xFFu);
  *b = static_cast<uint8_t>(rgb & 0xFFu);
}

// Hardware orientation: the WS2812B chain is wired so that the user-
// facing leftmost LED is the physical tail of the strip. Every pixel
// write goes through PhysIdx() so the rest of the controller (and the
// /api/lights mirror) can think in visual indices, where 0 is the
// leftmost LED and `g_count-1` is the rightmost.
inline uint32_t PhysIdx(uint32_t logical) {
  return g_count - 1 - logical;
}

// Single point of contact with led_strip_set_pixel — applies the
// hardware-orientation mapping above. All other paint helpers route
// through here.
void WritePixel(led_strip_handle_t strip, uint32_t idx, uint8_t r, uint8_t g,
                uint8_t b) {
  led_strip_set_pixel(strip, PhysIdx(idx), r, g, b);
}

// Apply the master brightness, then push one pixel to the strip.
void PushPixel(led_strip_handle_t strip, uint32_t idx, uint32_t rgb,
               uint8_t brightness) {
  uint8_t r = 0, g = 0, b = 0;
  Unpack(rgb, &r, &g, &b);
  r = led_curves::Scale(r, brightness);
  g = led_curves::Scale(g, brightness);
  b = led_curves::Scale(b, brightness);
  WritePixel(strip, idx, r, g, b);
}

// Paint + latch a uniform colour across the strip. Always honours the
// master brightness pref — effect handlers don't have to scale.
void PaintUniform(led_strip_handle_t strip, uint32_t rgb, uint8_t brightness) {
  for (uint32_t i = 0; i < g_count; ++i) {
    PushPixel(strip, i, rgb, brightness);
  }
  led_strip_refresh(strip);
}

// Restore the per-pixel "resting" mirror to the strip. Called whenever
// a one-shot effect finishes or kSetIdle is queued.
void PaintResting(led_strip_handle_t strip, uint8_t brightness) {
  for (uint32_t i = 0; i < g_count; ++i) {
    PushPixel(strip, i, g_resting_pixels[i], brightness);
  }
  led_strip_refresh(strip);
}

void PaintAllOff(led_strip_handle_t strip) {
  for (uint32_t i = 0; i < g_count; ++i) {
    WritePixel(strip, i, 0, 0, 0);
  }
  led_strip_refresh(strip);
}

// Prefs persistence — shared `settings` namespace, same key strings the
// WebUI PATCHes (and that GET /api/settings echoes back). Open-per-call
// so writes from effect setters don't fight the LED task; NVS itself
// has an internal mutex so concurrent opens are safe.
void PersistBrightness(uint8_t v) {
  Prefs p(prefs::kSettingsNs);
  p.SetU32(prefs::kLedBrightness, v);
  p.Commit();
}

void PersistBlockFlashColor(uint32_t rgb) {
  Prefs p(prefs::kSettingsNs);
  p.SetU32(prefs::kBlockFlashColor, rgb);
  p.Commit();
}

void PersistDisabled(bool v) {
  Prefs p(prefs::kSettingsNs);
  p.SetBool(prefs::kDisableLeds, v);
  p.Commit();
}

void PersistFlashUpdate(bool v) {
  Prefs p(prefs::kSettingsNs);
  p.SetBool(prefs::kLedFlashOnUpd, v);
  p.Commit();
}

void LoadPrefs() {
  // Pure-logic helper handles the legacy → settings migration so this
  // path stays IDF-free apart from the NvsPrefs adapters.
  settings::NvsPrefs settings_ns(prefs::kSettingsNs);
  settings::NvsPrefs legacy_ns(kLegacyNvsNamespace);
  const LedPrefsSnapshot s =
      ResolveLedPrefs(settings_ns, settings_ns, legacy_ns);
  if (s.migrated_from_legacy) {
    settings_ns.Commit();
    ESP_LOGI(kTag,
             "migrated LED prefs from legacy 'led' namespace into 'settings'");
    // Note: legacy `led/*` keys are intentionally left in place. The
    // Prefs component does not expose nvs_erase_key today, and a stray
    // legacy slot is harmless — ResolveLedPrefs always prefers settings
    // on subsequent boots once the values have been mirrored.
  }
  std::lock_guard<std::mutex> lk(g_state_mu);
  g_state.brightness = s.brightness;
  g_state.block_flash_color = s.block_flash_color;
  g_state.disabled = s.disabled;
  g_state.flash_on_update = s.flash_on_update;
}

// Effect implementations --------------------------------------------
// Each handler is allowed to block inside the LED task with vTaskDelay
// — the queue is short and callers treat PostLedEffect as fire-and-
// forget, same as the old Arduino firmware. Handlers MUST re-read
// brightness every frame so a live /api/lights/color tweak takes effect
// mid-flash.

uint8_t CurrentBrightness() {
  std::lock_guard<std::mutex> lk(g_state_mu);
  return g_state.brightness;
}

uint32_t CurrentBlockFlashColor() {
  std::lock_guard<std::mutex> lk(g_state_mu);
  return g_state.block_flash_color;
}

bool EffectsDisabled() {
  std::lock_guard<std::mutex> lk(g_state_mu);
  return g_state.disabled;
}

// True when DND is currently active. Copies the predicate out of the
// shared state to avoid holding g_state_mu while calling into an
// arbitrary caller-supplied function.
bool DndSuppressed() {
  std::function<bool()> pred;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    pred = g_suppressor;
  }
  return pred && pred();
}

// Short two-colour strobe. Matches LedHandler::blinkDelayTwoColor().
void PlayTwoColorBlink(led_strip_handle_t strip, uint32_t c1, uint32_t c2,
                       int d_ms, int times) {
  for (int j = 0; j < times; ++j) {
    PaintUniform(strip, c1, CurrentBrightness());
    vTaskDelay(pdMS_TO_TICKS(d_ms));
    PaintUniform(strip, c2, CurrentBrightness());
    vTaskDelay(pdMS_TO_TICKS(d_ms));
  }
  PaintAllOff(strip);
}

// Single-colour strobe (on / off). Matches blinkDelayColor().
void PlayColorBlink(led_strip_handle_t strip, uint32_t rgb, int d_ms,
                    int times) {
  for (int j = 0; j < times; ++j) {
    PaintUniform(strip, rgb, CurrentBrightness());
    vTaskDelay(pdMS_TO_TICKS(d_ms));
    PaintAllOff(strip);
    vTaskDelay(pdMS_TO_TICKS(d_ms));
  }
}

// Rapid multicolour strobe (identify). Matches LED_FLASH_IDENTIFY:
// red↔cyan twice, then green↔blue twice.
void PlayIdentify(led_strip_handle_t strip) {
  PlayTwoColorBlink(strip, PackRgb(255, 0, 0), PackRgb(0, 255, 255), 100, 2);
  PlayTwoColorBlink(strip, PackRgb(0, 255, 0), PackRgb(0, 0, 255), 100, 2);
}

// Quick bright pulse (zap receipt). Matches the old firmware's
// lightningStrike() shape — randomised yellow/purple per pixel.
void PlayZap(led_strip_handle_t strip) {
  const uint32_t kPurple = PackRgb(128, 0, 128);
  const uint32_t kYellow = PackRgb(255, 226, 41);
  // Fixed 8 strobes — old firmware randomised 7..10; we pick a midpoint
  // to keep the effect deterministic on bench testing without pulling
  // in esp_random() here.
  for (int n = 0; n < 8; ++n) {
    for (uint32_t i = 0; i < g_count; ++i) {
      PushPixel(strip, i, (i & 1) ? kPurple : kYellow, CurrentBrightness());
    }
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(30 + (n * 7) % 60));  // 30..90 ms
  }
}

// Slow red breath — approx. 2 s up / 2 s down. Indicates a data source
// is stuck but the device is alive.
void PlayDataError(led_strip_handle_t strip) {
  constexpr uint32_t kTotalTicks = 60;  // 60 * 33 ms ≈ 2 s
  for (uint32_t t = 0; t < kTotalTicks; ++t) {
    const uint8_t v = led_curves::Breath(255, t, kTotalTicks);
    PaintUniform(strip, PackRgb(v, 0, 0), CurrentBrightness());
    vTaskDelay(pdMS_TO_TICKS(33));
  }
  PaintAllOff(strip);
}

// Slow blue breath — "alive" marker. Matches LED_EFFECT_HEARTBEAT
// intent: two blue blinks in the old firmware, here replaced with a
// single breath pulse so the effect reads as "pulse" rather than
// "flash" at low brightness.
void PlayHeartbeat(led_strip_handle_t strip) {
  constexpr uint32_t kTotalTicks = 30;  // ~1 s total
  for (uint32_t t = 0; t < kTotalTicks; ++t) {
    const uint8_t v = led_curves::Breath(200, t, kTotalTicks);
    PaintUniform(strip, PackRgb(0, 0, v), CurrentBrightness());
    vTaskDelay(pdMS_TO_TICKS(33));
  }
  PaintAllOff(strip);
}

// WiFi spinner: single lit pixel walking the strip tail-to-head.
void PlayWifiConnecting(led_strip_handle_t strip) {
  const uint32_t kCyan = PackRgb(16, 197, 236);
  for (int32_t i = static_cast<int32_t>(g_count) - 1; i >= 0; --i) {
    for (uint32_t j = 0; j < g_count; ++j) {
      const uint32_t c = (static_cast<int32_t>(j) == i) ? kCyan : 0;
      PushPixel(strip, j, c, CurrentBrightness());
    }
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  PaintAllOff(strip);
}

// Single-pixel head→tail sweep with a linear per-step delay ramp.
// Does NOT clear when finished — leaves the last lit pixel held so
// callers can append a brake-light / handbrake frame on either end.
// `start_ms` is the delay AFTER the first frame; `end_ms` is the
// delay after the last frame.
void PlaySweepNoClear(led_strip_handle_t strip, uint32_t rgb, int start_ms,
                      int end_ms) {
  if (g_count == 0) return;
  const uint32_t n = g_count;
  const uint8_t bright = CurrentBrightness();
  for (uint32_t step = 0; step < n; ++step) {
    for (uint32_t j = 0; j < n; ++j) {
      PushPixel(strip, j, j == step ? rgb : 0, bright);
    }
    led_strip_refresh(strip);
    // Linear ramp from start_ms (step 0) to end_ms (step n-1).
    const int d = (n == 1) ? start_ms
                           : start_ms + (end_ms - start_ms) *
                                            static_cast<int>(step) /
                                            static_cast<int>(n - 1);
    vTaskDelay(pdMS_TO_TICKS(d));
  }
}

// Paint a single pixel of `rgb` at index `idx`, every other pixel off,
// hold for `hold_ms`. Used as the red anchor frames either side of the
// timer-pause / timer-resume sweeps.
void HoldSingle(led_strip_handle_t strip, uint32_t idx, uint32_t rgb,
                int hold_ms) {
  const uint8_t bright = CurrentBrightness();
  for (uint32_t j = 0; j < g_count; ++j) {
    PushPixel(strip, j, j == idx ? rgb : 0, bright);
  }
  led_strip_refresh(strip);
  vTaskDelay(pdMS_TO_TICKS(hold_ms));
}

// Pause: head→tail amber sweep that decelerates (~80 ms → ~300 ms per
// step), ending at the tail with a red "brake light" hold before
// fading off. Reads as a vehicle rolling to a stop.
void PlayTimerPause(led_strip_handle_t strip) {
  if (g_count == 0) return;
  PlaySweepNoClear(strip, PackRgb(255, 120, 0), 80, 300);
  HoldSingle(strip, g_count - 1, PackRgb(255, 0, 0), 500);
  PaintAllOff(strip);
}

// Resume: red "handbrake released" hold at the head, then a head→tail
// green sweep that accelerates (~300 ms → ~80 ms per step). Same
// direction as the pause sweep so the two read as braking + departing.
void PlayTimerResume(led_strip_handle_t strip) {
  if (g_count == 0) return;
  HoldSingle(strip, 0, PackRgb(255, 0, 0), 500);
  PlaySweepNoClear(strip, PackRgb(0, 200, 0), 300, 80);
  PaintAllOff(strip);
}

// Rainbow boot palette — one pass then clear.
void PlayPowerTest(led_strip_handle_t strip) {
  // 12 frames * 120 ms = ~1.4 s, roughly matches old rainbow(20) loop.
  for (size_t frame = 0; frame < 12; ++frame) {
    for (uint32_t i = 0; i < g_count; ++i) {
      const Rgb& c = kBootPalette[(frame + i) % kBootPalette.size()];
      PushPixel(strip, i, PackRgb(c.r, c.g, c.b), CurrentBrightness());
    }
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  PaintAllOff(strip);
}

// --- Task loop -----------------------------------------------------

void Task(void* arg) {
  auto* strip = static_cast<led_strip_handle_t>(arg);

  // Modes where the idle ticks do something (boot palette, boot-failed
  // solid); everything else is a one-shot we play inline and return to
  // idle immediately.
  enum class Mode : uint8_t { kBoot, kIdle, kBootFailed };
  Mode mode = Mode::kBoot;
  size_t frame = 0;

  while (true) {
    TickType_t wait;
    switch (mode) {
      case Mode::kBoot:
        wait = pdMS_TO_TICKS(250);
        break;
      case Mode::kBootFailed:
        wait = portMAX_DELAY;
        break;
      case Mode::kIdle:
        wait = portMAX_DELAY;
        break;
      default:
        wait = pdMS_TO_TICKS(500);
        break;
    }

    LedEffect ev;
    if (xQueueReceive(g_queue, &ev, wait) == pdTRUE) {
      // Global disable: drop everything but kSetIdle (which needs to
      // blank the strip and the resting mirror). DND behaves the same
      // — any effect except kSetIdle is silently swallowed so the
      // strip stays dark while the user's DND window is armed.
      if ((EffectsDisabled() || DndSuppressed()) && ev != LedEffect::kSetIdle) {
        continue;
      }
      // DND forces the idle path to paint black regardless of the
      // resting mirror; the mirror itself is preserved so disabling
      // DND later returns the strip to its prior colours without an
      // explicit /api/lights/color round-trip.
      if (DndSuppressed() && ev == LedEffect::kSetIdle) {
        mode = Mode::kIdle;
        PaintAllOff(strip);
        continue;
      }

      const uint8_t bright = CurrentBrightness();

      switch (ev) {
        case LedEffect::kSetBoot:
          mode = Mode::kBoot;
          frame = 0;
          break;

        case LedEffect::kSetIdle:
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kBlockFlash: {
          // 3x flash in blockFlashCol vs dim amber, matches
          // LED_FLASH_BLOCK_NOTIFY (blinkDelayTwoColor 250 ms, 3 times).
          const uint32_t col = CurrentBlockFlashColor();
          PlayTwoColorBlink(strip, col, PackRgb(8, 2, 0), 250, 3);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;
        }

        case LedEffect::kIdentify:
          PlayIdentify(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kZap:
          PlayZap(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kDataError:
          PlayDataError(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kDataBlockError:
          // Matches LED_DATA_BLOCK_ERROR — 2x purple blink, 150 ms.
          PlayColorBlink(strip, PackRgb(128, 0, 128), 150, 2);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kDataPriceError:
          // LED_DATA_PRICE_ERROR — 2x amber blink, 150 ms.
          PlayColorBlink(strip, PackRgb(177, 90, 31), 150, 2);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kFlashSuccess:
          // LED_FLASH_SUCCESS — 3x green blink, 150 ms.
          PlayColorBlink(strip, PackRgb(0, 255, 0), 150, 3);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kFlashError:
          // LED_FLASH_ERROR — 3x red blink, 250 ms.
          PlayColorBlink(strip, PackRgb(255, 0, 0), 250, 3);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kFlashUpdate: {
          // LED_FLASH_UPDATE — 3x two-colour (green↔yellow), 250 ms.
          // Gated on flashUpdate pref — effect is muted globally when
          // the user disables "flash on data update".
          bool enabled;
          {
            std::lock_guard<std::mutex> lk(g_state_mu);
            enabled = g_state.flash_on_update;
          }
          if (enabled) {
            PlayTwoColorBlink(strip, PackRgb(0, 230, 0), PackRgb(230, 230, 0),
                              250, 3);
          }
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;
        }

        case LedEffect::kHeartbeat:
          PlayHeartbeat(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kWifiConnecting:
          PlayWifiConnecting(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kWifiConnectError:
          // Red↔blue alternating, 3x @ 100 ms.
          PlayTwoColorBlink(strip, PackRgb(8, 161, 236), PackRgb(255, 0, 0),
                            100, 3);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kWifiConnectSuccess:
          // Triple green flash.
          PlayColorBlink(strip, PackRgb(0, 255, 0), 150, 3);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kWifiWaitForConfig:
          // Twin-blue flash, 1x @ 100 ms.
          PlayTwoColorBlink(strip, PackRgb(8, 161, 236), PackRgb(156, 225, 240),
                            100, 1);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kBootFailed:
          mode = Mode::kBootFailed;
          PaintUniform(strip, PackRgb(255, 0, 0), bright);
          break;

        case LedEffect::kPowerTest:
          PlayPowerTest(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kTimerPause:
          PlayTimerPause(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;

        case LedEffect::kTimerResume:
          PlayTimerResume(strip);
          mode = Mode::kIdle;
          PaintResting(strip, bright);
          break;
      }
    }

    // Animated ticks: drive the boot palette forward one frame per
    // 250 ms. kBootFailed latches the red and waits on the queue.
    switch (mode) {
      case Mode::kBoot: {
        const uint8_t bright = CurrentBrightness();
        for (uint32_t i = 0; i < g_count; ++i) {
          const Rgb& c = kBootPalette[(frame + i) % kBootPalette.size()];
          PushPixel(strip, i, PackRgb(c.r, c.g, c.b), bright);
        }
        led_strip_refresh(strip);
        ++frame;
        break;
      }
      case Mode::kBootFailed:
      case Mode::kIdle:
        // Nothing per-tick — one-shot effects repainted the resting
        // state above; the queue wait parks us until the next event.
        break;
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
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_state.pixel_count = count;
  }
  LoadPrefs();
  // Production sdkconfig silences asserts, so check explicitly. If the
  // queue or the task can't be created (heap exhaustion), leave g_queue
  // null — PostLedEffect already short-circuits on that, so the LED
  // subsystem degrades to a no-op rather than spawning a task that
  // would xQueueReceive(nullptr).
  g_queue = xQueueCreate(8, sizeof(LedEffect));
  if (g_queue == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate failed; LEDs disabled");
    return;
  }
  led_strip_handle_t strip = InitStrip(pin, count);
  g_strip = strip;
  if (xTaskCreate(Task, "leds", 4096, strip, tskIDLE_PRIORITY + 1, nullptr) !=
      pdPASS) {
    ESP_LOGE(kTag, "xTaskCreate failed; LEDs disabled");
    vQueueDelete(g_queue);
    g_queue = nullptr;
    return;
  }
  LedState s;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    s = g_state;
  }
  ESP_LOGI(kTag,
           "init: pin=%d count=%u brightness=%u blockFlash=0x%06lX disable=%d "
           "flashUpdate=%d",
           static_cast<int>(pin), static_cast<unsigned>(count),
           static_cast<unsigned>(s.brightness),
           static_cast<unsigned long>(s.block_flash_color), s.disabled ? 1 : 0,
           s.flash_on_update ? 1 : 0);
}

void PostLedEffect(LedEffect ev) {
  if (g_queue == nullptr) return;
  // Pre-filter at the post site to match LedHandler::queueEffect's
  // drop-on-DND behaviour in the old firmware — queue slots are only 8
  // deep, so dropping here keeps genuine events from being starved out
  // by a burst while DND is armed. kSetIdle is always allowed so the
  // strip can be forcibly cleared / repainted.
  if (ev != LedEffect::kSetIdle && DndSuppressed()) return;
  if (xQueueSend(g_queue, &ev, 0) != pdTRUE) {
    queue_metrics::RecordDrop(queue_metrics::Queue::kLed);
  }
}

// --- State + prefs accessors (called from HTTP task) ---------------

LedState GetLedState() {
  std::lock_guard<std::mutex> lk(g_state_mu);
  LedState s = g_state;
  // g_state.pixels is the resting mirror; make sure we echo whichever
  // was painted most recently by SetLedSolidColor / SetLedPixels.
  for (uint32_t i = 0;
       i < s.pixel_count && i < sizeof(s.pixels) / sizeof(s.pixels[0]); ++i) {
    s.pixels[i] = g_resting_pixels[i];
  }
  return s;
}

void SetLedBrightness(uint8_t brightness) {
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_state.brightness = brightness;
  }
  PersistBrightness(brightness);
}

void SetBlockFlashColor(uint32_t rgb) {
  rgb &= 0x00FFFFFFu;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_state.block_flash_color = rgb;
  }
  PersistBlockFlashColor(rgb);
}

void SetLedDisabled(bool disabled) {
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_state.disabled = disabled;
  }
  PersistDisabled(disabled);
  if (disabled) {
    // Force-clear the strip right now rather than wait for the next
    // event. The task loop will naturally paint black on its next
    // iteration via PaintResting, but we also clear the resting mirror
    // here so a re-enable doesn't flash the old colour.
    std::lock_guard<std::mutex> lk(g_state_mu);
    for (auto& p : g_resting_pixels) p = 0;
    PostLedEffect(LedEffect::kSetIdle);
  }
}

void SetLedFlashOnUpdate(bool enabled) {
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_state.flash_on_update = enabled;
  }
  PersistFlashUpdate(enabled);
}

void SetLedSolidColor(uint32_t rgb) {
  rgb &= 0x00FFFFFFu;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    for (uint32_t i = 0; i < g_count; ++i) g_resting_pixels[i] = rgb;
    // /api/lights/color?c=off uses this path to clear the strip. Keep
    // the disabled flag in sync so the WebUI toggle tracks reality.
    if (rgb == 0) {
      g_state.disabled = true;
    } else {
      g_state.disabled = false;
    }
  }
  // Persist the mute flag only — the resting colour isn't stored on
  // this controller (matches the IDF scope; old firmware kept
  // ledColor_<i> keys for restore, which we intentionally skip here).
  PersistDisabled(rgb == 0);
  // Repaint via the task — kSetIdle drops through to PaintResting.
  PostLedEffect(LedEffect::kSetIdle);
}

void SetLedPixels(const uint32_t* rgb_array, uint32_t count) {
  if (!rgb_array) return;
  const uint32_t n = std::min<uint32_t>(count, g_count);
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    for (uint32_t i = 0; i < n; ++i) {
      g_resting_pixels[i] = rgb_array[i] & 0x00FFFFFFu;
    }
    // "All zero" short-circuits to off — matches old firmware's
    // setLights() ledStatus flag.
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
      if (g_resting_pixels[i] != 0) {
        any = true;
        break;
      }
    }
    g_state.disabled = !any;
  }
  PostLedEffect(LedEffect::kSetIdle);
}

bool LedsReady() {
  return g_queue != nullptr;
}

void SetLedActiveSuppressor(std::function<bool()> predicate) {
  std::lock_guard<std::mutex> lk(g_state_mu);
  g_suppressor = std::move(predicate);
}

// --- OTA progress paint path ---------------------------------------
// These paint directly on the strip under g_direct_mu rather than
// queueing effects so the progress indicator updates in lock-step with
// the httpd worker's write loop. They deliberately ignore the
// `disabled` + DND predicates: the user initiated an OTA and needs to
// see progress even if they normally mute the LEDs.

void ShowOtaProgressLedCount(int lit_count) {
  if (!g_strip || g_count == 0) return;
  const uint8_t bright = CurrentBrightness();
  const uint32_t green = PackRgb(0, 255, 0);
  std::lock_guard<std::mutex> lk(g_direct_mu);
  const int clamped =
      std::max(0, std::min(lit_count, static_cast<int>(g_count)));
  for (uint32_t i = 0; i < g_count; ++i) {
    if (static_cast<int>(i) < clamped) {
      PushPixel(g_strip, i, green, bright);
    } else {
      WritePixel(g_strip, i, 0, 0, 0);
    }
  }
  led_strip_refresh(g_strip);
}

void ShowOtaProgressIndeterminate() {
  // Content-Length was missing — no fraction to display. Light pixel 0
  // only so the user sees "something is happening" without implying a
  // concrete bar position.
  ShowOtaProgressLedCount(1);
}

void PlayOtaCompletionBlink(int times, int d_ms) {
  if (!g_strip || g_count == 0) return;
  const uint8_t bright = CurrentBrightness();
  const uint32_t green = PackRgb(0, 255, 0);
  std::lock_guard<std::mutex> lk(g_direct_mu);
  for (int t = 0; t < times; ++t) {
    for (uint32_t i = 0; i < g_count; ++i) {
      PushPixel(g_strip, i, green, bright);
    }
    led_strip_refresh(g_strip);
    vTaskDelay(pdMS_TO_TICKS(d_ms));
    for (uint32_t i = 0; i < g_count; ++i) {
      WritePixel(g_strip, i, 0, 0, 0);
    }
    led_strip_refresh(g_strip);
    vTaskDelay(pdMS_TO_TICKS(d_ms));
  }
  // Leave all LEDs lit green as a "done" marker until esp_restart fires.
  for (uint32_t i = 0; i < g_count; ++i) {
    PushPixel(g_strip, i, green, bright);
  }
  led_strip_refresh(g_strip);
}

}  // namespace btclock
