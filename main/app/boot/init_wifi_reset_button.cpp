#include "app/boot/init_wifi_reset_button.hpp"

#include "app/app_ctx.hpp"
#include "board/board.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/led_controller.hpp"
#include "mcp23017.hpp"
#include "prefs.hpp"
#include "settings/factory_reset.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {

constexpr const char* kTag = "wifi-reset-btn";

// 3 s of continuous hold to trigger the wipe. Long enough that a casual
// finger-rest at power-up doesn't fire, short enough that a user
// expecting v3's behaviour doesn't give up before it triggers.
constexpr int kHoldMs = 3000;
constexpr int kPollMs = 50;

// MCP pin backing the device's front-of-device "button 1" label.
// The pin depends on board::kButtonsInvertedDefault XOR'd with the
// inverseButtons pref — same composition that ButtonReader does at
// runtime, kept in sync so the boot-time check reads the SAME physical
// pin the user calls "button 1".
//
//   final inverted=false: button 1 → ButtonId::k0 → GPA3 (Rev A/B default)
//   final inverted=true:  button 1 → ButtonId::k0 → GPA0 (V8 default)
//
// We read the MCP directly here — ButtonReader isn't running this
// early in boot — but we still want the user-facing button label to
// stay consistent with what the same button does at runtime.
uint8_t Button1Pin() {
  Prefs settings(prefs::kSettingsNs);
  const bool pref = settings.GetBool(prefs::kInverseButtons, false);
  const bool inverted = board::kButtonsInvertedDefault ^ pref;
  return inverted ? 0 : 3;
}

bool IsButton1Pressed(Mcp23017& mcp, uint8_t pin) {
  uint16_t port = 0;
  if (mcp.ReadPort(&port) != ESP_OK) return false;
  // Active-low: 0 = pressed.
  return (port & (1u << pin)) == 0;
}

}  // namespace

void MaybeWifiResetAtBoot(AppCtx& ctx) {
  if (!ctx.mcp.has_value()) return;
  Mcp23017& mcp = *ctx.mcp;
  const uint8_t pin = Button1Pin();

  if (!IsButton1Pressed(mcp, pin)) return;

  ESP_LOGW(kTag, "button 1 held at boot; arming wifi reset (hold %d ms)",
           kHoldMs);
  // Red flash so the user sees the device has noticed the hold. Posted
  // every poll iteration so it stays visible across the whole window —
  // a single post would expire halfway through.
  PostLedEffect(LedEffect::kFlashError);

  int elapsed_ms = 0;
  while (elapsed_ms < kHoldMs) {
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
    elapsed_ms += kPollMs;
    if (!IsButton1Pressed(mcp, pin)) {
      ESP_LOGI(kTag, "button 1 released after %d ms; aborting wifi reset",
               elapsed_ms);
      return;
    }
    // Re-post on every ~500 ms tick so the LED stays clearly active for
    // the full window without flooding the queue.
    if (elapsed_ms % 500 == 0) {
      PostLedEffect(LedEffect::kFlashError);
    }
  }

  ESP_LOGW(kTag, "button 1 held %d ms; wiping STA credentials and rebooting",
           kHoldMs);
  settings::PerformWifiReset();  // [[noreturn]] — wipes net/ssid + net/pw,
                                 // clears wifiConfigured, then esp_restart.
}

}  // namespace btclock
