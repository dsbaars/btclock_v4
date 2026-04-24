// NeoPixel controller — FreeRTOS task + event queue.
//
// Callers post LedEvents from any task (button handler, data-source
// callback, main loop); a dedicated task consumes the queue and drives
// the WS2812B strip. Matches the event-queue pattern the production
// firmware uses for the same purpose (src/lib/drivers/leds/led_handler.cpp).
//
// Effect catalog (subset of the old firmware's LED_* constants — only
// the ones actually called from production paths are ported; see grep
// of LED_FLASH_* / LED_EFFECT_* in src/ for the provenance):
//
//   kSetBoot            rainbow palette cycle, drawn until another event
//   kSetIdle            LEDs off (restore state)
//   kBlockFlash         block-flash pulse, `blockFlashCol` vs dim
//   kIdentify           rapid multicolour flash, fires on /api/identify
//   kZap                quick bright pulse (Nostr zap receipt)
//   kDataError          slow red breath (generic stuck data source)
//   kDataBlockError     quick purple blink (block source stuck)
//   kDataPriceError     quick amber blink (price source stuck)
//   kFlashSuccess       quick green blink (save-ok confirmation)
//   kFlashError         quick red blink (generic error)
//   kFlashUpdate        yellow-green blink (flash-on-data-update)
//   kHeartbeat          single slow blue blink (alive marker)
//   kWifiConnecting     blue spinner sweep
//   kWifiConnectError   red/blue alternating flash
//   kWifiConnectSuccess triple green flash
//   kWifiWaitForConfig  twin-blue flash (AP mode)
//   kBootFailed         solid red (boot sanity failure)
//   kPowerTest          rainbow scan then clear
//
// Global toggles (NVS namespace `"led"`, matches old firmware key
// strings where practical; see app/led_controller.cpp for the mapping):
//   brightness     uint8_t    0..255,  default 128
//   blockFlashCol  uint32_t   0xRRGGBB, default 0xE04300 (orange)
//   disable        bool       mute all effects, default false
//   flashUpdate    bool       flash on data update, default true

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "driver/gpio.h"

namespace btclock {

enum class LedEffect : uint8_t {
  kSetBoot = 0,
  kSetIdle,
  kBlockFlash,
  kIdentify,
  kZap,
  kDataError,
  kDataBlockError,
  kDataPriceError,
  kFlashSuccess,
  kFlashError,
  kFlashUpdate,
  kHeartbeat,
  kWifiConnecting,
  kWifiConnectError,
  kWifiConnectSuccess,
  kWifiWaitForConfig,
  kBootFailed,
  kPowerTest,
};

// Snapshot of current prefs + per-pixel colour mirror, returned to the
// control server so /api/lights and /api/status can echo the state.
struct LedState {
  uint8_t brightness = 128;
  uint32_t block_flash_color = 0xE04300;
  bool disabled = false;
  bool flash_on_update = true;
  // Per-pixel colour mirror (packed 0x00RRGGBB). Length matches the
  // NEOPIXEL_COUNT configured at InitLeds() time.
  uint32_t pixels[8] = {0};
  uint32_t pixel_count = 0;
};

// Create the event queue and start the LED task. Call once at boot;
// subsequent calls are undefined. No shutdown path — the task runs
// until reboot. Loads prefs from NVS namespace `"led"` before starting.
void InitLeds(gpio_num_t pin, uint32_t count);

// Post an effect to the LED task. Thread-safe. Non-blocking; drops if
// the queue is full (8 slots). No-op when the `disable` pref is true
// (effect is simply swallowed; the task checks per-frame).
//
// Backwards-compat shim: accepts a LedEvent alias so existing call
// sites in main.cpp keep compiling. Prefer LedEffect for new code.
void PostLedEffect(LedEffect ev);

// Alias — kept for the 3-event API in main.cpp / existing call sites.
using LedEvent = LedEffect;
inline void PostLedEvent(LedEffect ev) { PostLedEffect(ev); }

// --- State + prefs API for the control server ---
// All setters persist to NVS and apply to the runtime state on the
// caller's thread (atomic under the controller's mutex). Getters return
// the current runtime value — cheap, no NVS hit.

// Read the full state snapshot (prefs + per-pixel mirror).
LedState GetLedState();

// Update the master brightness (NVS key "brightness"). Clamps to 0..255
// and applies to subsequent pixel writes.
void SetLedBrightness(uint8_t brightness);

// Update the block-flash color (NVS key "blockFlashCol"). Stored as a
// packed 0x00RRGGBB uint32.
void SetBlockFlashColor(uint32_t rgb);

// Global mute — when true, PostLedEffect drops events and the strip is
// forced off (NVS key "disable").
void SetLedDisabled(bool disabled);

// Enable / disable the flash-on-data-update effect (NVS key
// "flashUpdate"). Callers that invoke kFlashUpdate should gate on this
// first; the controller honours it defensively too.
void SetLedFlashOnUpdate(bool enabled);

// Paint a solid colour across every pixel, bypassing the effect catalog
// and saving the result as the current "resting" state the idle mode
// restores to. Used by /api/lights/color. Caller passes a packed 0xRRGGBB.
// Setting colour=0 is equivalent to /api/lights/off — mute state is
// updated so the WebUI reflects it.
void SetLedSolidColor(uint32_t rgb);

// Paint per-pixel colours, index 0 first. `count` is clamped to the
// configured pixel count. Mirrors the old firmware's /api/lights/set.
void SetLedPixels(const uint32_t* rgb_array, uint32_t count);

// True if the controller was initialised and its queue is live.
bool LedsReady();

// Install a predicate the controller consults before accepting an
// effect post (and from inside the task before painting). When it
// returns true the strip is treated as if `disable` were set: the
// effect queue drains silently and the resting mirror paints black.
// Installed once at boot from main.cpp and pointed at the DND
// subsystem — kept as a std::function rather than a direct dependency
// so the LED component doesn't have to link against `dnd` (which
// pulls NVS and mutex state).
void SetLedActiveSuppressor(std::function<bool()> predicate);

// --- OTA progress indicator -------------------------------------------
// Paint `lit_count` green LEDs from index 0 followed by off LEDs for the
// rest of the strip. Clamps `lit_count` to [0, pixel_count]. Bypasses
// the effect queue and the DND / disabled predicates — the OTA UX is
// a user-initiated, time-bounded action that must remain visible. The
// resting mirror is NOT touched, so a successful OTA finishes with an
// `esp_restart()` and a cold start repaints the normal colours; a
// failed OTA reverts to the resting mirror via a SetLedIdle() call from
// the caller.
void ShowOtaProgressLedCount(int lit_count);

// Indeterminate indicator — a single green pixel at index 0, rest off.
// Used when Content-Length is missing on the upload so the write side
// can't compute a fraction. The indicator is static (no pulsing / no
// animation) because the HTTP worker thread is blocked in recv() for
// most of the write; an animated indicator would freeze visually.
void ShowOtaProgressIndeterminate();

// Completion blink — flash all pixels green `times` times at `d_ms`
// cadence. Blocks the caller for roughly times * 2 * d_ms. Called from
// the HTTP worker after esp_ota_set_boot_partition succeeds but before
// the scheduled reboot fires, so the user sees a clear "done" signal
// on the LED strip even when the EPD hasn't been re-painted.
void PlayOtaCompletionBlink(int times = 3, int d_ms = 150);

}  // namespace btclock
