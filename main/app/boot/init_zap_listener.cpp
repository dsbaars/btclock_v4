#include "app/boot/init_zap_listener.hpp"

#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "nostr/nostr_data_source.hpp"
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
constexpr const char* kTag = "btclock";

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
       zap_notify_ptr, zap_pending_ptr](const nostr::ZapListener::ZapInfo& z) {
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

  // Reject bare-hostname / https:// `nostrRelay` values that survive
  // from before the PATCH-side scheme guard. RelayClient would
  // otherwise log "Invalid uri" on Start() and the listener would
  // silently never connect — keep it inert so the boot path still
  // log-explains why. Each pubkey must already be 64-char hex (PATCH
  // validator enforces); recheck here so a hand-edited NVS that drops
  // a stray space or a partial hex doesn't leak through.
  const bool relay_scheme_ok = zap_cfg.relay_url.rfind("wss://", 0) == 0 ||
                               zap_cfg.relay_url.rfind("ws://", 0) == 0;
  bool all_pubkeys_ok = !zap_cfg.zap_pubkeys.empty();
  for (const auto& pk : zap_cfg.zap_pubkeys) {
    if (pk.size() != 64) {
      all_pubkeys_ok = false;
      break;
    }
  }
  if (!(zap_cfg.enabled && !zap_cfg.relay_url.empty() && all_pubkeys_ok &&
        relay_scheme_ok)) {
    ESP_LOGI(kTag,
             "zap listener disabled (enable=%d relay=%s pubs=%zu/%s "
             "scheme_ok=%d)",
             zap_cfg.enabled ? 1 : 0,
             zap_cfg.relay_url.empty() ? "<empty>" : "set",
             zap_cfg.zap_pubkeys.size(), all_pubkeys_ok ? "ok" : "invalid",
             relay_scheme_ok ? 1 : 0);
    return;
  }

  // Try to share the Nostr data source's RelayClient + SubscriptionManager
  // when both the data source and the zap listener point at the same
  // relay. NIP-01 supports multiple subscriptions per WSS, so the second
  // RelayClient is pure overhead (~30+ KB internal SRAM + heap
  // fragmentation that pinned espLargestFreeBlock at 7 KB and silently
  // broke the EPD render path). Falls back to the original separate-WSS
  // path when the URLs differ or the data source is absent.
  nostr::SubscriptionManager* shared_subs = nullptr;
  if (ctx.nostr_source != nullptr &&
      ShouldShareNostrRelay(ctx.nostr_source->relay_url(), zap_cfg.relay_url)) {
    shared_subs = ctx.nostr_source->subs();
  }
  // Log helper: first pubkey's 8-char prefix + total count keeps the
  // boot log informative without dumping all 8 hex strings on V8 boards.
  auto pubkeys_log = [](const std::vector<std::string>& pks) {
    std::string s =
        pks.empty() ? std::string("<none>") : pks.front().substr(0, 8) + "…";
    if (pks.size() > 1) s += "+" + std::to_string(pks.size() - 1);
    return s;
  };

  if (shared_subs != nullptr) {
    ctx.zap_listener = std::make_unique<nostr::ZapListener>(
        *shared_subs, std::string("zap"), zap_cfg.zap_pubkeys);
    ctx.zap_pubkeys_current = zap_cfg.zap_pubkeys;
    BindOnZap(ctx);
    ctx.zap_listener->Start();
    ESP_LOGI(kTag,
             "zap listener enabled (shared WSS via nostr data source): "
             "relay=%s pubs=%s flashLed=%d flashFl=%d",
             zap_cfg.relay_url.c_str(),
             pubkeys_log(zap_cfg.zap_pubkeys).c_str(),
             ctx.flash_on_zap_enabled.load() ? 1 : 0,
             ctx.flash_frontlight_on_zap_enabled.load() ? 1 : 0);
    return;
  }

  ctx.zap_relay = std::make_unique<nostr::RelayClient>(zap_cfg.relay_url);
  ctx.zap_subs = std::make_unique<nostr::SubscriptionManager>(*ctx.zap_relay);
  ctx.zap_listener = std::make_unique<nostr::ZapListener>(
      *ctx.zap_subs, std::string("zap"), zap_cfg.zap_pubkeys);
  ctx.zap_pubkeys_current = zap_cfg.zap_pubkeys;

  BindOnZap(ctx);
  if (auto err = ctx.zap_relay->Start(); err != ESP_OK) {
    ESP_LOGE(kTag,
             "zap relay Start() failed: %s (relay=%s) — disabling listener",
             esp_err_to_name(err), zap_cfg.relay_url.c_str());
    ctx.zap_listener.reset();
    ctx.zap_subs.reset();
    ctx.zap_relay.reset();
    return;
  }
  ctx.zap_listener->Start();
  ESP_LOGI(kTag,
           "zap listener enabled (dedicated WSS): relay=%s pubs=%s "
           "flashLed=%d flashFl=%d",
           zap_cfg.relay_url.c_str(), pubkeys_log(zap_cfg.zap_pubkeys).c_str(),
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
  //
  // Two valid wiring shapes after InitZapListener:
  //   A) dedicated WSS — ctx.zap_relay + ctx.zap_subs both set, listener
  //      borrows ctx.zap_subs.
  //   B) shared WSS — both ctx.zap_relay and ctx.zap_subs are null, the
  //      listener borrows ctx.nostr_source->subs() instead. We must NOT
  //      treat (B) as "listener unset" — only the absence of zap_listener
  //      itself signals that.
  if (!ctx.zap_listener) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: listener unset, "
             "skip Stop+Start (master toggle requires reboot)");
    return;
  }
  // Resolve which SubscriptionManager the listener is currently using —
  // either the dedicated zap_subs (shape A) or the data source's subs
  // (shape B). Pubkey rotation rebuilds the listener against the same
  // manager so the existing socket stays up.
  nostr::SubscriptionManager* active_subs = ctx.zap_subs.get();
  if (active_subs == nullptr && ctx.nostr_source != nullptr) {
    active_subs = ctx.nostr_source->subs();
  }
  if (active_subs == nullptr) {
    ESP_LOGW(kTag,
             "RefreshZapListenerSettings: no SubscriptionManager available "
             "(neither dedicated nor shared); skipping pubkey rotation");
    return;
  }

  // Pubkey list unchanged → toggling LED/frontlight/screen-notify only
  // updates the atomics already done above. Stop/Start would tear down
  // the relay subscription unnecessarily. Comparison is order-sensitive
  // — a deliberate reorder by the user counts as a change so the REQ
  // filter reflects the new ordering on the wire.
  if (zap_cfg.zap_pubkeys == ctx.zap_pubkeys_current) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: atomics refreshed "
             "(led=%d fl=%d notify=%d)",
             zap_cfg.led_flash_on_zap ? 1 : 0,
             zap_cfg.frontlight_flash_on_zap ? 1 : 0,
             zap_cfg.zap_screen_notify ? 1 : 0);
    return;
  }

  // Pubkey list changed: ZapListener binds the REQ filter at
  // construction, so rebuild against the same SubscriptionManager.
  // Schema rejects bad-length / non-hex / over-cap pubkeys at PATCH
  // time so the invariants below should already hold; the defensive
  // checks catch the empty-list and hand-edited-NVS paths.
  if (zap_cfg.zap_pubkeys.empty()) {
    ESP_LOGW(kTag,
             "RefreshZapListenerSettings: new pubkey list empty, "
             "keeping previous subscription");
    return;
  }
  for (const auto& pk : zap_cfg.zap_pubkeys) {
    if (pk.size() != 64) {
      ESP_LOGW(kTag,
               "RefreshZapListenerSettings: new pubkey invalid (len=%zu), "
               "keeping previous subscription",
               pk.size());
      return;
    }
  }
  ctx.zap_listener->Stop();
  ctx.zap_listener = std::make_unique<nostr::ZapListener>(
      *active_subs, std::string("zap"), zap_cfg.zap_pubkeys);
  ctx.zap_pubkeys_current = zap_cfg.zap_pubkeys;
  BindOnZap(ctx);
  if (!ctx.zap_listener->Start()) {
    ESP_LOGW(kTag,
             "RefreshZapListenerSettings: Start() failed for %zu pubkeys "
             "(first=%s…)",
             zap_cfg.zap_pubkeys.size(),
             zap_cfg.zap_pubkeys.front().substr(0, 8).c_str());
    return;
  }
  ESP_LOGI(kTag,
           "RefreshZapListenerSettings: rotated to %zu pubkeys (first=%s…)",
           zap_cfg.zap_pubkeys.size(),
           zap_cfg.zap_pubkeys.front().substr(0, 8).c_str());
}

}  // namespace btclock
