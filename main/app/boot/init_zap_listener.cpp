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
#include "nostr/zap_id_lru.hpp"
#include "nostr/zap_listener.hpp"
#include "prefs.hpp"
#include "settings/nostr_config.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";

// Bind the on-zap callback for one ZapListener. Factored out so
// RefreshZapListenerSettings can rebuild the listeners with new pubkeys
// and re-attach the same callback shape without duplicating the
// captures. The shared ZapIdLru lives on AppCtx so callbacks bound
// across multiple relays cooperate: the first relay to deliver a given
// kind-9735 receipt wins, the rest see MarkFresh()=false and drop.
void BindOnZap(AppCtx& ctx, nostr::ZapListener& listener) {
  // Neither LED nor frontlight effects are fired from the relay-worker
  // callback. Both are dispatched from event_loop.cpp's zap-notify
  // branch AFTER sm.Render() paints the zap overlay, so the user sees
  // the screen change first and the LED ring + staggered frontlight
  // pulse fire together (mirrors the new-block notification timing).
  // Only the snapshot update + the zap-pending flag stay here — those
  // need to land before the main loop wakes.
  DataHub* hub_ptr = ctx.hub.get();
  TaskHandle_t main_task = ctx.main_task;
  auto* zap_notify_ptr = &ctx.zap_notify_screen_enabled;
  auto* zap_pending_ptr = &ctx.zap_notify_pending;
  nostr::ZapIdLru* lru = ctx.zap_id_lru.get();

  listener.SetOnZap([hub_ptr, main_task, zap_notify_ptr, zap_pending_ptr,
                     lru](const nostr::ZapListener::ZapInfo& z) {
    // Multi-relay dedup: when the same NIP-57 receipt arrives over
    // sibling relays the first call wins and the rest drop. Empty id
    // (relay misbehaved) treats as fresh — the renderer's per-event
    // bolt11 + amount overlay still benefits from the screen-overlay
    // single-fire even when we can't dedupe by id.
    const std::string_view eid_view =
        z.raw ? std::string_view(z.raw->id) : std::string_view();
    if (lru && !lru->MarkFresh(eid_view)) {
      ESP_LOGD(kTag, "zap dropped (duplicate id=%.8s)",
               eid_view.empty() ? "?" : eid_view.data());
      return;
    }
    const uint64_t sats = z.amount_msat / 1000ULL;
    const std::string eid = z.raw ? z.raw->id.substr(0, 8) : std::string("?");
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
    // nostrZapNotify gates the screen override (and, transitively,
    // the LED + frontlight effects fired from the dispatcher when
    // the overlay paints). The per-effect gates ledFlashOnZap and
    // flFlashOnZap are evaluated at the dispatcher site so a live
    // PATCH lands without a reboot — see event_loop.cpp's zap-
    // notify branch.
    if (zap_notify_ptr->load()) {
      // Signal the main loop to flip ScreenManager into the
      // zap overlay. The hub Report above also wakes main_task
      // via the on-update callback; the pending flag picks
      // that wake up and dispatches SetZapNotify.
      zap_pending_ptr->store(true);
      xTaskNotifyGive(main_task);
    }
  });
}

// Locate the matching NostrDataSource for a zap relay URL — used to
// share the WSS via NIP-01 multi-sub instead of opening a second
// socket. Returns null when no data source is wired for that URL (or
// dataSource != 2). Comparison goes through ShouldShareNostrRelay so
// the trailing-slash + lowercase normalisation is identical to the
// validator's gate.
nostr::SubscriptionManager* FindSiblingSubs(AppCtx& ctx,
                                            const std::string& url) {
  for (auto* ds : ctx.nostr_sources) {
    if (ds && ShouldShareNostrRelay(ds->relay_url(), url)) return ds->subs();
  }
  return nullptr;
}

// Validate the zap config common gates (master enable, pubkey shape).
// Per-relay scheme is checked at the iteration site so a single bad
// URL in the list doesn't disable every relay.
bool ZapConfigBasicallyValid(const settings::ZapListenerConfig& zap_cfg) {
  if (!zap_cfg.enabled) return false;
  if (zap_cfg.relay_urls.empty()) return false;
  if (zap_cfg.zap_pubkeys.empty()) return false;
  for (const auto& pk : zap_cfg.zap_pubkeys) {
    if (pk.size() != 64) return false;
  }
  return true;
}
}  // namespace

void InitZapListener(AppCtx& ctx) {
  if (!ctx.wifi || ctx.wifi->is_ap_mode()) return;

  // All zap-listener prefs (relay URLs, zap pubkey, the flash gates and
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

  if (!ZapConfigBasicallyValid(zap_cfg)) {
    ESP_LOGI(kTag, "zap listener disabled (enable=%d relays=%u pubs=%zu)",
             zap_cfg.enabled ? 1 : 0,
             static_cast<unsigned>(zap_cfg.relay_urls.size()),
             zap_cfg.zap_pubkeys.size());
    return;
  }

  // Construct the shared event-id LRU once before any listener fires.
  // BindOnZap captures it by raw pointer so future reconstructions in
  // RefreshZapListenerSettings keep the same dedup window — a freshly
  // arrived duplicate from a relay that reconnected mid-rotation still
  // hits the LRU instead of slipping past as a "first sight".
  if (!ctx.zap_id_lru) ctx.zap_id_lru = std::make_unique<nostr::ZapIdLru>();

  // Log helper: first pubkey's 8-char prefix + total count keeps the
  // boot log informative without dumping all 8 hex strings on V8 boards.
  auto pubkeys_log = [](const std::vector<std::string>& pks) {
    std::string s =
        pks.empty() ? std::string("<none>") : pks.front().substr(0, 8) + "…";
    if (pks.size() > 1) s += "+" + std::to_string(pks.size() - 1);
    return s;
  };

  // Reserve sized to the worst-case all-dedicated path; the shared path
  // leaves zap_relays / zap_subs short, but reserving up-front avoids
  // intermediate reallocations that would invalidate listener pointers
  // mid-loop.
  ctx.zap_listeners.reserve(zap_cfg.relay_urls.size());
  ctx.zap_relays.reserve(zap_cfg.relay_urls.size());
  ctx.zap_subs.reserve(zap_cfg.relay_urls.size());

  std::size_t shared_count = 0, dedicated_count = 0;
  for (std::size_t i = 0; i < zap_cfg.relay_urls.size(); ++i) {
    const std::string& url = zap_cfg.relay_urls[i];
    const bool scheme_ok =
        url.rfind("wss://", 0) == 0 || url.rfind("ws://", 0) == 0;
    if (!scheme_ok) {
      ESP_LOGW(kTag, "zap relay skipped (bad scheme): %s", url.c_str());
      continue;
    }

    // Per-relay sub-id keeps NIP-01 dispatch unambiguous when one
    // SubscriptionManager carries both data + zap subs.
    const std::string sub_id = "zap-" + std::to_string(i);

    nostr::SubscriptionManager* shared_subs = FindSiblingSubs(ctx, url);
    if (shared_subs != nullptr) {
      // Ride an existing data-source RelayClient. NIP-01 supports
      // multiple subs per WSS, so the second RelayClient would be pure
      // overhead (~13 KB internal SRAM + ~24 KB PSRAM measured Rev B)
      // and the matching largest-block fragmentation that pinned
      // espLargestFreeBlock at 7 KB and silently broke the EPD render
      // path on long-running devices.
      auto listener = std::make_unique<nostr::ZapListener>(*shared_subs, sub_id,
                                                           zap_cfg.zap_pubkeys);
      BindOnZap(ctx, *listener);
      listener->Start();
      ctx.zap_listeners.push_back(std::move(listener));
      ++shared_count;
      ESP_LOGI(kTag, "zap listener (shared WSS): relay=%s", url.c_str());
      continue;
    }

    // Dedicated WSS — either dataSource != 2 (no NostrDataSource at
    // all) or the URL doesn't match any sibling. Open our own socket.
    auto relay = std::make_unique<nostr::RelayClient>(url);
    auto subs = std::make_unique<nostr::SubscriptionManager>(*relay);
    auto listener = std::make_unique<nostr::ZapListener>(*subs, sub_id,
                                                         zap_cfg.zap_pubkeys);
    BindOnZap(ctx, *listener);
    if (auto err = relay->Start(); err != ESP_OK) {
      ESP_LOGE(kTag,
               "zap relay Start() failed: %s (relay=%s) — skipping this relay",
               esp_err_to_name(err), url.c_str());
      // Don't push partial state — let the listener / relay / subs go
      // out of scope and free immediately so we don't leak a half-
      // wired entry into ctx.zap_*.
      continue;
    }
    listener->Start();
    ctx.zap_relays.push_back(std::move(relay));
    ctx.zap_subs.push_back(std::move(subs));
    ctx.zap_listeners.push_back(std::move(listener));
    ++dedicated_count;
    ESP_LOGI(kTag, "zap listener (dedicated WSS): relay=%s", url.c_str());
  }

  ctx.zap_pubkeys_current = zap_cfg.zap_pubkeys;
  ESP_LOGI(kTag,
           "zap listeners up: shared=%u dedicated=%u pubs=%s flashLed=%d "
           "flashFl=%d",
           static_cast<unsigned>(shared_count),
           static_cast<unsigned>(dedicated_count),
           pubkeys_log(zap_cfg.zap_pubkeys).c_str(),
           ctx.flash_on_zap_enabled.load() ? 1 : 0,
           ctx.flash_frontlight_on_zap_enabled.load() ? 1 : 0);
}

void RefreshZapListenerSettings(AppCtx& ctx) {
  // Re-read every runtime-editable nostr key from the canonical
  // "settings" NVS namespace and refresh the in-memory atomics so the
  // on-zap callback (which loads them on each receipt) sees the new
  // values immediately. nostrRelay(s) / nostrPubKey are boot_only and
  // are intentionally NOT applied here — the schema's rebootRequired
  // response steers the user through the reboot path.
  settings::NvsPrefs settings_prefs(prefs::kSettingsNs);
  const auto zap_cfg = settings::ReadZapListenerConfig(settings_prefs);
  ctx.flash_on_zap_enabled.store(zap_cfg.led_flash_on_zap);
  ctx.flash_frontlight_on_zap_enabled.store(zap_cfg.frontlight_flash_on_zap);
  ctx.zap_notify_screen_enabled.store(zap_cfg.zap_screen_notify);
  ctx.zap_screen_auto_restore.store(zap_cfg.zap_screen_auto_restore);

  // Listeners were never wired (boot disabled it because the master
  // toggle was off, or every relay URL was invalid). PATCH-toggling the
  // master back on requires reboot to construct the RelayClient + WS
  // task — keeping that out of the runtime path is a deliberate scope
  // cut for bd btclock_v4-aw5/q1l.
  if (ctx.zap_listeners.empty()) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: no listeners wired, "
             "skip Stop+Start (master toggle requires reboot)");
    return;
  }

  // Pubkey list unchanged → toggling LED/frontlight/screen-notify only
  // updates the atomics already done above. Stop/Start would tear down
  // every relay subscription unnecessarily.
  if (zap_cfg.zap_pubkeys == ctx.zap_pubkeys_current) {
    ESP_LOGI(kTag,
             "RefreshZapListenerSettings: atomics refreshed "
             "(led=%d fl=%d notify=%d)",
             zap_cfg.led_flash_on_zap ? 1 : 0,
             zap_cfg.frontlight_flash_on_zap ? 1 : 0,
             zap_cfg.zap_screen_notify ? 1 : 0);
    return;
  }

  // Defensive checks for the new pubkey list. Schema rejects bad-length
  // / non-hex / over-cap pubkeys at PATCH time so the invariants below
  // should already hold; the checks catch the empty-list and hand-
  // edited-NVS paths.
  if (zap_cfg.zap_pubkeys.empty()) {
    ESP_LOGW(kTag,
             "RefreshZapListenerSettings: new pubkey list empty, "
             "keeping previous subscriptions");
    return;
  }
  for (const auto& pk : zap_cfg.zap_pubkeys) {
    if (pk.size() != 64) {
      ESP_LOGW(kTag,
               "RefreshZapListenerSettings: new pubkey invalid (len=%zu), "
               "keeping previous subscriptions",
               pk.size());
      return;
    }
  }

  // Pubkey list changed: each ZapListener binds the REQ filter at
  // construction, so rebuild every listener against its existing
  // SubscriptionManager. We avoid tearing the underlying RelayClients
  // down — just stop+rebuild the listeners against the same managers.
  // Index parallel to ctx.zap_listeners so the new vector ends up the
  // same length and order.
  std::vector<std::unique_ptr<nostr::ZapListener>> rebuilt;
  rebuilt.reserve(ctx.zap_listeners.size());
  for (std::size_t i = 0; i < ctx.zap_listeners.size(); ++i) {
    auto& old = ctx.zap_listeners[i];
    if (!old) continue;
    // Each listener's SubscriptionManager is whichever one it borrowed
    // at construction — could be a sibling NostrDataSource's subs (the
    // shared path) or our own ctx.zap_subs[k] (dedicated). Snapshot the
    // ref before destroying the listener: ZapListener holds the manager
    // by reference, but we don't get to reach back through it. Stop the
    // old listener so its CLOSE frame fires before we open the new sub.
    old->Stop();
  }
  ctx.zap_listeners.clear();

  // Re-walk the relay URLs and re-attach. This mirrors InitZapListener's
  // loop but reuses the existing RelayClient/Subs (in the dedicated
  // path) and the existing data-source subs (in the shared path).
  std::size_t dedicated_idx = 0;
  for (std::size_t i = 0; i < zap_cfg.relay_urls.size(); ++i) {
    const std::string& url = zap_cfg.relay_urls[i];
    const std::string sub_id = "zap-" + std::to_string(i);

    nostr::SubscriptionManager* subs = FindSiblingSubs(ctx, url);
    if (subs == nullptr) {
      // Dedicated path — find the matching ctx.zap_relays entry.
      // Order is preserved by the original InitZapListener loop, so the
      // dedicated entries appear in URL order.
      while (
          dedicated_idx < ctx.zap_relays.size() &&
          !ShouldShareNostrRelay(ctx.zap_relays[dedicated_idx]->url(), url)) {
        ++dedicated_idx;
      }
      if (dedicated_idx >= ctx.zap_relays.size()) {
        ESP_LOGW(kTag,
                 "RefreshZapListenerSettings: no dedicated subs for %s, "
                 "skipping rebuild",
                 url.c_str());
        continue;
      }
      subs = ctx.zap_subs[dedicated_idx].get();
      ++dedicated_idx;
    }
    auto listener = std::make_unique<nostr::ZapListener>(*subs, sub_id,
                                                         zap_cfg.zap_pubkeys);
    BindOnZap(ctx, *listener);
    if (!listener->Start()) {
      ESP_LOGW(kTag,
               "RefreshZapListenerSettings: Start() failed for relay=%s "
               "(first pub=%s…)",
               url.c_str(), zap_cfg.zap_pubkeys.front().substr(0, 8).c_str());
      continue;
    }
    rebuilt.push_back(std::move(listener));
  }
  ctx.zap_listeners = std::move(rebuilt);
  ctx.zap_pubkeys_current = zap_cfg.zap_pubkeys;
  ESP_LOGI(
      kTag,
      "RefreshZapListenerSettings: rotated %u listener(s) to %zu pubkey(s) "
      "(first=%s…)",
      static_cast<unsigned>(ctx.zap_listeners.size()),
      zap_cfg.zap_pubkeys.size(),
      zap_cfg.zap_pubkeys.front().substr(0, 8).c_str());
}

}  // namespace btclock
