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
#include "settings/nostr_config.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";

// Bind the on-zap callback. Factored out so RefreshZapListenerSettings
// can rebuild the ZapListener with a new pubkey and re-attach the same
// callback shape without duplicating the captures.
void BindOnZap(AppCtx& ctx) {
  if (!ctx.zap_listener) return;
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
}
}  // namespace

void InitZapListener(AppCtx& ctx) {
  if (!ctx.wifi || ctx.wifi->is_ap_mode()) return;

  // All zap-listener prefs (relay URL, zap pubkey, the flash gates and
  // the screen-notify master toggle) live in the canonical "settings"
  // NVS namespace where /api/settings PATCH writes them. Earlier this
  // file opened a separate "nostr" namespace with shorthand keys
  // ("zapEnable"/"zapRelay"/"zapPubkey"/"flashOnZap") so a WebUI PATCH
  // silently no-op'd (bd btclock_v4-q1l). One read pass through
  // ReadZapListenerConfig keeps the full set in lock-step with the
  // schema; the on_nostr_changed control-server hook re-runs it on
  // PATCH for the runtime-editable subset (nostrZapPubkey,
  // nostrZapNotify, ledFlashOnZap, flFlashOnZap, scrnRestoreZap).
  settings::NvsPrefs settings_prefs(prefs::kSettingsNs);
  const auto zap_cfg = settings::ReadZapListenerConfig(settings_prefs);
  ctx.flash_on_zap_enabled.store(zap_cfg.led_flash_on_zap);
  ctx.flash_frontlight_on_zap_enabled.store(zap_cfg.frontlight_flash_on_zap);
  ctx.zap_notify_screen_enabled.store(zap_cfg.zap_screen_notify);
  ctx.zap_screen_auto_restore.store(zap_cfg.zap_screen_auto_restore);

  if (!(zap_cfg.enabled && !zap_cfg.relay_url.empty() &&
        zap_cfg.zap_pubkey.size() == 64)) {
    ESP_LOGI(kTag, "zap listener disabled (enable=%d relay=%s pub=%s)",
             zap_cfg.enabled ? 1 : 0,
             zap_cfg.relay_url.empty() ? "<empty>" : "set",
             zap_cfg.zap_pubkey.size() == 64 ? "set" : "<invalid>");
    return;
  }

  ctx.zap_relay = std::make_unique<nostr::RelayClient>(zap_cfg.relay_url);
  ctx.zap_subs =
      std::make_unique<nostr::SubscriptionManager>(*ctx.zap_relay);
  ctx.zap_listener = std::make_unique<nostr::ZapListener>(
      *ctx.zap_subs, std::string("zap"), zap_cfg.zap_pubkey);
  ctx.zap_pubkey_current = zap_cfg.zap_pubkey;

  BindOnZap(ctx);
  if (auto err = ctx.zap_relay->Start(); err != ESP_OK) {
    ESP_LOGE(kTag, "zap relay Start() failed: %s (relay=%s) — disabling listener",
             esp_err_to_name(err), zap_cfg.relay_url.c_str());
    ctx.zap_listener.reset();
    ctx.zap_subs.reset();
    ctx.zap_relay.reset();
    return;
  }
  ctx.zap_listener->Start();
  ESP_LOGI(kTag,
           "zap listener enabled: relay=%s pub=%s… flashLed=%d flashFl=%d",
           zap_cfg.relay_url.c_str(),
           zap_cfg.zap_pubkey.substr(0, 8).c_str(),
           ctx.flash_on_zap_enabled.load() ? 1 : 0,
           ctx.flash_frontlight_on_zap_enabled.load() ? 1 : 0);
}

void RefreshZapListenerSettings(AppCtx& ctx) {
  // Re-read every runtime-editable nostr key from the canonical
  // "settings" namespace and refresh the in-memory atomics so the
  // on-zap callback (which loads them on each receipt) sees the new
  // values immediately. nostrRelay / nostrPubKey are boot_only and
  // are intentionally NOT applied here — the schema's rebootRequired
  // response steers the user through the reboot path.
  settings::NvsPrefs settings_prefs(prefs::kSettingsNs);
  const auto zap_cfg = settings::ReadZapListenerConfig(settings_prefs);
  ctx.flash_on_zap_enabled.store(zap_cfg.led_flash_on_zap);
  ctx.flash_frontlight_on_zap_enabled.store(zap_cfg.frontlight_flash_on_zap);
  ctx.zap_notify_screen_enabled.store(zap_cfg.zap_screen_notify);
  ctx.zap_screen_auto_restore.store(zap_cfg.zap_screen_auto_restore);

  // Listener was never wired (boot disabled it because the master
  // toggle was off, or the pubkey was invalid). PATCH-toggling the
  // master back on requires reboot to construct the RelayClient + WS
  // task — keeping that out of the runtime path is a deliberate scope
  // cut for bd btclock_v4-aw5/q1l (RelayClient bring-up isn't safe
  // from the httpd worker thread without more synchronisation).
  if (!ctx.zap_listener || !ctx.zap_subs) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: listener unset, "
             "skip Stop+Start (master toggle requires reboot)");
    return;
  }

  // Pubkey unchanged → toggling LED/frontlight/screen-notify only
  // updates the atomics already done above. Stop/Start would tear
  // down the relay subscription unnecessarily.
  const bool pubkey_changed =
      (zap_cfg.zap_pubkey != ctx.zap_pubkey_current);
  if (!pubkey_changed) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: atomics refreshed "
             "(led=%d fl=%d notify=%d)",
             zap_cfg.led_flash_on_zap ? 1 : 0,
             zap_cfg.frontlight_flash_on_zap ? 1 : 0,
             zap_cfg.zap_screen_notify ? 1 : 0);
    return;
  }

  // Pubkey changed: a SubscriptionManager REQ filter is bound at
  // construction (the recipient_pubkey_hex passed into ZapListener),
  // so we have to drop the listener and rebuild with the new pubkey.
  // Schema rejects bad-length / non-hex pubkeys at PATCH time so the
  // 64-char invariant should already hold; the defensive check below
  // catches the empty-string-clears-the-field path that ApplyPatch
  // does allow.
  if (zap_cfg.zap_pubkey.size() != 64) {
    ESP_LOGW(kTag,
             "RefreshZapListenerSettings: new pubkey invalid "
             "(len=%zu), keeping previous subscription",
             zap_cfg.zap_pubkey.size());
    return;
  }
  ctx.zap_listener->Stop();
  ctx.zap_listener = std::make_unique<nostr::ZapListener>(
      *ctx.zap_subs, std::string("zap"), zap_cfg.zap_pubkey);
  ctx.zap_pubkey_current = zap_cfg.zap_pubkey;
  BindOnZap(ctx);
  ctx.zap_listener->Start();
  ESP_LOGI(kTag,
           "RefreshZapListenerSettings: pubkey rotated to %s…",
           zap_cfg.zap_pubkey.substr(0, 8).c_str());
}

}  // namespace btclock
