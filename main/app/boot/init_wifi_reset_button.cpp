#include "app/boot/init_wifi_reset_button.hpp"

#include "app/app_ctx.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/led_controller.hpp"
#include "mcp23017.hpp"
#include "settings/factory_reset.hpp"

namespace btclock {
namespace {

constexpr const char* kTag = "wifi-reset-btn";

// 3 s of continuous hold to trigger the wipe. Long enough that a casual
// finger-rest at power-up doesn't fire, short enough that a user
// expecting v3's behaviour doesn't give up before it triggers.
constexpr int kHoldMs = 3000;
constexpr int kPollMs = 50;

// Physical pin to monitor: MCP1 GPA0. Per buttons.hpp this is the
// leftmost physical button ("button 1 in user-facing numbering"). We
// read the pin directly (not via ButtonReader) because the reader
// task isn't started this early and we'd rather not spend RAM on a
// short-lived poll task. Active-low: 0 = pressed.
constexpr uint8_t kButton1Pin = 0;

bool IsButton1Pressed(Mcp23017& mcp) {
  uint16_t port = 0;
  if (mcp.ReadPort(&port) != ESP_OK) return false;
  return (port & (1u << kButton1Pin)) == 0;
}

}  // namespace

void MaybeWifiResetAtBoot(AppCtx& ctx) {
  if (!ctx.mcp.has_value()) return;
  Mcp23017& mcp = *ctx.mcp;

  if (!IsButton1Pressed(mcp)) return;

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
    if (!IsButton1Pressed(mcp)) {
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
