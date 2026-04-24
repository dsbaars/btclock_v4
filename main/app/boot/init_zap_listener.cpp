#include "app/boot/init_zap_listener.hpp"

#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"
#include "nostr/zap_listener.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";
}  // namespace

void InitZapListener(AppCtx& ctx) {
  if (!ctx.wifi || ctx.wifi->is_ap_mode()) return;

  Prefs zap_prefs("nostr");
  const bool zap_enable = zap_prefs.GetBool("zapEnable", true);
  const std::string zap_relay_url =
      zap_prefs.GetString("zapRelay", "wss://relay.primal.net");
  const std::string zap_pub = zap_prefs.GetString(
      "zapPubkey",
      "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422");
  ctx.flash_on_zap_enabled.store(zap_prefs.GetBool("flashOnZap", true));

  // Frontlight flash-on-zap lives in the frontlight pref namespace to
  // match the old firmware (src/lib/system/pref_keys.hpp::FlFlashOnZap),
  // so re-using it keeps settings migration from Arduino straightforward.
  {
    Prefs fl_prefs("frontlight");
    ctx.flash_frontlight_on_zap_enabled.store(
        fl_prefs.GetBool("flFlashOnZap", false));
  }
  // Notification-screen gates live in the "settings" namespace
  // alongside the other renderer-behaviour flags so a WebUI PATCH
  // round-trips without poking two namespaces.
  {
    Prefs settings_prefs(prefs::kSettingsNs);
    ctx.zap_notify_screen_enabled.store(
        settings_prefs.GetBool(prefs::kNostrZapNotify, true));
    ctx.zap_screen_auto_restore.store(
        settings_prefs.GetBool(prefs::kScrnRestoreZap, true));
  }

  if (!(zap_enable && !zap_relay_url.empty() && zap_pub.size() == 64)) {
    ESP_LOGI(kTag, "zap listener disabled (enable=%d relay=%s pub=%s)",
             zap_enable ? 1 : 0,
             zap_relay_url.empty() ? "<empty>" : "set",
             zap_pub.size() == 64 ? "set" : "<invalid>");
    return;
  }

  ctx.zap_relay = std::make_unique<nostr::RelayClient>(zap_relay_url);
  ctx.zap_subs =
      std::make_unique<nostr::SubscriptionManager>(*ctx.zap_relay);
  ctx.zap_listener = std::make_unique<nostr::ZapListener>(
      *ctx.zap_subs, std::string("zap"), zap_pub);

  FrontlightController* fl_ptr = ctx.frontlight.get();
  DataHub* hub_ptr = ctx.hub.get();
  TaskHandle_t main_task = ctx.main_task;
  auto* flash_on_zap_ptr = &ctx.flash_on_zap_enabled;
  auto* flash_fl_on_zap_ptr = &ctx.flash_frontlight_on_zap_enabled;
  auto* zap_notify_ptr = &ctx.zap_notify_screen_enabled;
  auto* zap_pending_ptr = &ctx.zap_notify_pending;

  ctx.zap_listener->SetOnZap(
      [fl_ptr, hub_ptr, main_task, flash_on_zap_ptr, flash_fl_on_zap_ptr,
       zap_notify_ptr, zap_pending_ptr](
          const nostr::ZapListener::ZapInfo& z) {
        const uint64_t sats = z.amount_msat / 1000ULL;
        const std::string eid =
            z.raw ? z.raw->id.substr(0, 8) : std::string("?");
        ESP_LOGI(kTag, "zap: %llu sats id=%s…",
                 static_cast<unsigned long long>(sats), eid.c_str());
        ESP_LOGD(kTag, "zap bolt11: %s", z.bolt11.c_str());
        // Always update the snapshot so /api/status can echo the
        // latest receipt regardless of whether we pop the screen —
        // matches the spec choice of "nostrZapNotify gates the
        // override + LED flash only, not the data side".
        if (hub_ptr) {
          DataSnapshot patch;
          patch.latest_zap.amount_sats = static_cast<int64_t>(sats);
          patch.latest_zap.message = z.content;
          patch.latest_zap.received_ms = MsNow();
          hub_ptr->Report(patch);
        }
        // Notify + LED flash only fire when the user hasn't
        // disabled them. Keep both gated by the same pref so a
        // "quiet" user experience is opt-in via a single toggle.
        if (zap_notify_ptr->load()) {
          if (flash_on_zap_ptr->load()) {
            PostLedEffect(LedEffect::kZap);
          }
          if (fl_ptr && flash_fl_on_zap_ptr->load()) {
            fl_ptr->ZapFlash();
          }
          // Signal the main loop to flip ScreenManager into the
          // zap overlay. The hub Report above also wakes main_task
          // via the on-update callback; the pending flag picks
          // that wake up and dispatches SetZapNotify.
          zap_pending_ptr->store(true);
          xTaskNotifyGive(main_task);
        }
      });
  ESP_ERROR_CHECK(ctx.zap_relay->Start());
  ctx.zap_listener->Start();
  ESP_LOGI(kTag,
           "zap listener enabled: relay=%s pub=%s… flashLed=%d flashFl=%d",
           zap_relay_url.c_str(), zap_pub.substr(0, 8).c_str(),
           ctx.flash_on_zap_enabled.load() ? 1 : 0,
           ctx.flash_frontlight_on_zap_enabled.load() ? 1 : 0);
}

}  // namespace btclock
