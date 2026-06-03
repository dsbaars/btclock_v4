#include "app/boot/init_nwc.hpp"

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"
#include "nwc/client.hpp"
#include "nwc/jsonrpc.hpp"
#include "nwc/queue.hpp"
#include "nwc/uri.hpp"
#include "prefs.hpp"
#include "settings/nvs_store.hpp"
#include "settings/nwc_config.hpp"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";

// Cap kNwcLastBalSat at u32::max sats (~42.9 BTC). Storing as msat
// would overflow at ~4.3 BTC. The boot-time cache is "best effort"
// for the first render; real wallets above the cap just show "—"
// until the first live get_balance lands.
constexpr int64_t kBalanceCacheCapSats = 4'000'000'000LL;
// NVS-write debounce. Writes happen on the relay-worker task; the
// ESP timer running the periodic poll already paces requests to
// nwcRefreshSecs (>=15s), but a wallet with notifications enabled
// can trigger an extra write per payment. 60 s keeps the NVS wear
// bounded.
constexpr int64_t kBalanceCacheMinIntervalMs = 60'000;

// Notification worker stack. The heavy path here is NIP-44 v2
// decrypt (ChaCha20 + HMAC-SHA-256, ~1 KiB scratch) + cJSON parse
// (~2 KiB arena) + a small dispatch. 8 KiB is comfortable with room
// for log frames and HMAC scratch. The WS RX task (~3-4 KiB) blew
// up on this same workload — see bd lwf.9 for the crash dump.
constexpr uint32_t kNotifWorkerStackBytes = 8 * 1024;
// One step BELOW the relay-WS task. Lower priority keeps the WS
// callback path responsive even when the worker is mid-decrypt; the
// queue handoff is fast either way (microseconds-level mutex).
constexpr UBaseType_t kNotifWorkerPriority = 4;

void NwcNotifWorker(void* arg) {
  auto* ctx = static_cast<AppCtx*>(arg);
  for (;;) {
    nwc::RawNotification raw;
    if (!ctx->nwc_notif_queue) break;  // teardown — only happens on shutdown
    if (ctx->nwc_notif_queue->WaitPop(raw, /*timeout_ms=*/1000)) {
      if (ctx->nwc_client) {
        ctx->nwc_client->DispatchRawNotification(raw);
      }
    } else if (!ctx->nwc_notif_queue) {
      break;
    }
    // On timeout, just loop. The worker stays alive for the process
    // lifetime; we never tear down NWC dynamically.
  }
  ctx->nwc_notif_worker = nullptr;
  vTaskDelete(nullptr);
}

// Rebind the refresh ticker to a new period. Recreates the
// underlying esp_timer; the cb stays the same. Returns the new
// handle or nullptr on failure.
esp_timer_handle_t StartRefreshTimer(AppCtx& ctx, uint32_t period_secs) {
  if (period_secs == 0) return nullptr;
  esp_timer_create_args_t args = {};
  // Cheap callback only — raise the pending flag and wake the main
  // task. The heavy NIP-44 encrypt + schnorr sign + JSON build that
  // `RequestGetBalance()` performs would not fit on the esp_timer
  // task's ~3.5 KiB stack and used to abort with a stack overflow
  // every nwcRefreshSecs.
  args.callback = [](void* arg) {
    auto* c = static_cast<AppCtx*>(arg);
    c->nwc_refresh_pending.store(true);
    if (c->main_task) xTaskNotifyGive(c->main_task);
  };
  args.arg = &ctx;
  args.name = "nwc_poll";
  args.dispatch_method = ESP_TIMER_TASK;
  esp_timer_handle_t h = nullptr;
  if (esp_timer_create(&args, &h) != ESP_OK) return nullptr;
  if (esp_timer_start_periodic(
          h, static_cast<uint64_t>(period_secs) * 1'000'000ULL) != ESP_OK) {
    esp_timer_delete(h);
    return nullptr;
  }
  return h;
}

void StopRefreshTimer(AppCtx& ctx) {
  if (!ctx.nwc_refresh_timer) return;
  auto* h = static_cast<esp_timer_handle_t>(ctx.nwc_refresh_timer);
  esp_timer_stop(h);
  esp_timer_delete(h);
  ctx.nwc_refresh_timer = nullptr;
}

}  // namespace

void InitNwc(AppCtx& ctx) {
  if (!ctx.wifi || ctx.wifi->is_ap_mode()) return;

  settings::NvsPrefs prefs(prefs::kSettingsNs);
  const auto cfg = settings::ReadNwcConfig(prefs);
  ctx.nwc_flash_on_payment_enabled.store(cfg.flash_on_payment);
  ctx.nwc_notify_auto_restore.store(ctx.zap_screen_auto_restore.load());

  if (!cfg.enabled || cfg.uri.empty() || !cfg.parsed_ok) {
    ESP_LOGI(kTag, "nwc disabled (enabled=%d uri_set=%d parsed=%d)",
             cfg.enabled ? 1 : 0, cfg.uri.empty() ? 0 : 1,
             cfg.parsed_ok ? 1 : 0);
    return;
  }
  if (cfg.parsed.relays.empty()) {
    ESP_LOGW(kTag, "nwc disabled: parsed URI has no relays");
    return;
  }

  // First relay wins — most pairing URIs ship a single relay anyway.
  // A multi-relay URI would need a relay pool; out of scope for v1
  // and would burn additional internal SRAM. The schema reject in
  // ApplyPatch keeps the budget invariant intact.
  const std::string& url = cfg.parsed.relays.front();
  ctx.nwc_relay = std::make_unique<nostr::RelayClient>(url);
  ctx.nwc_subs = std::make_unique<nostr::SubscriptionManager>(*ctx.nwc_relay);

  // Wire publish/subscribe shims pointing at the dedicated RelayClient
  // and SubscriptionManager. Captures are raw pointers; AppCtx outlives
  // every callback because Stop happens before AppCtx destruction.
  nostr::RelayClient* relay_ptr = ctx.nwc_relay.get();
  nostr::SubscriptionManager* subs_ptr = ctx.nwc_subs.get();
  auto publish = [relay_ptr](const char* data, size_t len) -> bool {
    return relay_ptr->SendText(data, len);
  };
  auto subscribe = [subs_ptr](const std::string& sub_id,
                              const nostr::Filter& f) {
    subs_ptr->Subscribe(sub_id, f);
  };
  auto unsubscribe = [subs_ptr](const std::string& sub_id) {
    subs_ptr->Unsubscribe(sub_id);
  };

  ctx.nwc_client = std::make_unique<nwc::NwcClient>(
      cfg.parsed, std::move(publish), std::move(subscribe),
      std::move(unsubscribe));

  // Stand up the bounded notification queue + worker BEFORE
  // `Start()` opens the relay — otherwise the first kind 23197 that
  // races the worker spawn would land on the legacy synchronous path
  // and re-trip the WS-task stack overflow.
  ctx.nwc_notif_queue = std::make_unique<nwc::NotificationQueue>();
  nwc::NotificationQueue* queue_ptr = ctx.nwc_notif_queue.get();
  ctx.nwc_client->SetNotifEnqueueFn(
      [queue_ptr](nwc::RawNotification&& raw) -> bool {
        return queue_ptr->TryPush(std::move(raw));
      });
  if (xTaskCreate(NwcNotifWorker, "nwc_notify",
                  kNotifWorkerStackBytes / sizeof(StackType_t), &ctx,
                  kNotifWorkerPriority, &ctx.nwc_notif_worker) != pdPASS) {
    ESP_LOGE(kTag,
             "nwc_notify worker xTaskCreate failed — leaving NWC disabled");
    ctx.nwc_client.reset();
    ctx.nwc_subs.reset();
    ctx.nwc_relay.reset();
    ctx.nwc_notif_queue.reset();
    return;
  }
  // esp_fill_random uses the SoC HW RNG. time(nullptr) returns 0 when
  // the SNTP sync hasn't landed yet; the relay rejects events with
  // created_at=0, so fall back to the monotonic clock (boot-relative).
  // Both are "advisory" timestamps for the relay — what matters for
  // signature validity is the cryptographic content, not the wallclock.
  ctx.nwc_client->SetRandomFn(
      [](uint8_t* out, size_t n) { esp_fill_random(out, n); });
  ctx.nwc_client->SetNowFn([]() -> int64_t {
    const std::time_t now = std::time(nullptr);
    if (now > 1'700'000'000LL) return static_cast<int64_t>(now);
    return static_cast<int64_t>(esp_timer_get_time() / 1'000'000LL);
  });

  // Route the manager's event dispatch back into the NwcClient. The
  // manager hands every EVENT frame to its on_event_ callback; we
  // forward each one through HandleEvent which routes by kind.
  nwc::NwcClient* client_ptr = ctx.nwc_client.get();
  ctx.nwc_subs->SetOnEvent(
      [client_ptr](const std::string& /*sub_id*/, const nostr::Event& ev) {
        client_ptr->HandleEvent(ev);
      });

  // Seed nwc_balance_msat from the runtime-state NVS cache so the
  // balance screen has a value before the first live response. Stored
  // as u32 whole sats; convert back to msat for the snapshot.
  DataHub* hub_ptr = ctx.hub.get();
  if (hub_ptr) {
    Prefs rt(prefs::kRuntimeStateNs);
    const uint32_t cached_sats = rt.GetU32(prefs::kNwcLastBalSat, 0);
    if (cached_sats > 0) {
      DataSnapshot patch;
      patch.nwc_balance_msat = static_cast<int64_t>(cached_sats) * 1000LL;
      hub_ptr->Report(patch);
      ESP_LOGI(kTag, "nwc balance cache → %u sats",
               static_cast<unsigned>(cached_sats));
    }
  }

  TaskHandle_t main_task = ctx.main_task;
  auto* notify_pending = &ctx.nwc_notify_pending;
  AppCtx* ctx_ptr = &ctx;
  const uint32_t refresh_secs = cfg.refresh_secs;

  ctx.nwc_client->SetOnReady([ctx_ptr, refresh_secs](const nwc::InfoEvent& ev) {
    ESP_LOGI(
        kTag, "nwc ready: enc=%s methods=%zu notifs=%zu — first poll",
        ev.encryption.empty() ? "(default)" : ev.encryption.front().c_str(),
        ev.methods.size(), ev.notifications.size());
    if (ctx_ptr->nwc_client) ctx_ptr->nwc_client->RequestGetBalance();
    if (ctx_ptr->nwc_refresh_timer == nullptr) {
      ctx_ptr->nwc_refresh_timer = StartRefreshTimer(*ctx_ptr, refresh_secs);
    }
    // The one-shot list_transactions poll fires AFTER the first
    // balance response lands (see SetOnBalance below) — NwcClient
    // tracks only a single inflight request_id at a time, so
    // back-to-back RequestGetBalance + RequestListTransactions would
    // overwrite the inflight bookkeeping and drop the balance reply.
  });

  ctx.nwc_client->SetOnBalance([hub_ptr, ctx_ptr](uint64_t balance_msat) {
    static int64_t last_persist_ms = INT64_MIN;
    static int64_t last_persisted_sats = -1;
    const int64_t sats = static_cast<int64_t>(balance_msat / 1000ULL);
    if (hub_ptr) {
      DataSnapshot patch;
      patch.nwc_balance_msat = static_cast<int64_t>(balance_msat);
      hub_ptr->Report(patch);
    }
    const int64_t now = MsNow();
    if (sats <= kBalanceCacheCapSats && (last_persisted_sats != sats) &&
        (now - last_persist_ms >= kBalanceCacheMinIntervalMs)) {
      Prefs rt(prefs::kRuntimeStateNs);
      rt.SetU32(prefs::kNwcLastBalSat, static_cast<uint32_t>(sats));
      last_persist_ms = now;
      last_persisted_sats = sats;
    }
    ESP_LOGD(kTag, "nwc balance: %llu msat (%lld sats)",
             static_cast<unsigned long long>(balance_msat),
             static_cast<long long>(sats));

    // One-shot list_transactions poll after the first balance reply.
    // NIP-47 push notifications only deliver events received AFTER
    // subscribe — payments that landed while the device was offline
    // (mid-OTA, power off, post-crash) need a poll to surface. The
    // wallet returns the [now-600s, now] window; we fan each settled
    // tx out via the existing on_payment_ path so the overlay
    // pipeline (LED flash, screen overlay, snapshot patch) reuses.
    // Chained off on_balance so the inflight-request bookkeeping
    // serializes naturally — NwcClient holds a single inflight id at
    // a time. Gated on SNTP being landed; otherwise `from_secs`
    // would be a boot-relative seconds value the wallet rejects.
    static std::atomic<bool> boot_poll_done{false};
    bool expected = false;
    if (!boot_poll_done.compare_exchange_strong(expected, true)) return;
    const std::time_t wallclock = std::time(nullptr);
    if (wallclock <= 1'700'000'000LL) {
      ESP_LOGW(kTag, "nwc boot poll skipped: SNTP not landed (now=%lld)",
               static_cast<long long>(wallclock));
      return;
    }
    const int64_t from_secs = static_cast<int64_t>(wallclock) - 600;
    ESP_LOGI(kTag, "nwc boot poll: list_transactions since %lld (last 10 min)",
             static_cast<long long>(from_secs));
    if (ctx_ptr && ctx_ptr->nwc_client) {
      ctx_ptr->nwc_client->RequestListTransactions(from_secs, /*limit=*/20);
    }
  });

  ctx.nwc_client->SetOnPayment([hub_ptr, main_task, notify_pending](
                                   const nwc::PaymentNotification& p) {
    const int64_t sats = static_cast<int64_t>(p.amount_msat / 1000ULL);
#if defined(BTCLOCK_DIAG_NWC_FLASH) && BTCLOCK_DIAG_NWC_FLASH
    // WARN so it survives the WARN-max build log level; counts every
    // on_payment_ fan-out (the dedup makes this fire once per payment).
    ESP_LOGW(kTag, "nwc payment: dir=%u amount=%lld sats",
             static_cast<unsigned>(p.direction), static_cast<long long>(sats));
#else
    ESP_LOGI(kTag, "nwc payment: dir=%u amount=%lld sats",
             static_cast<unsigned>(p.direction), static_cast<long long>(sats));
#endif
    if (hub_ptr) {
      DataSnapshot patch;
      patch.nwc_last_payment.direction = static_cast<uint8_t>(p.direction);
      patch.nwc_last_payment.amount_sats = sats;
      patch.nwc_last_payment.description = p.description;
      patch.nwc_last_payment.received_ms = MsNow();
      hub_ptr->Report(patch);
    }
    notify_pending->store(true);
    if (main_task) xTaskNotifyGive(main_task);
  });

  if (auto err = ctx.nwc_relay->Start(); err != ESP_OK) {
    ESP_LOGE(kTag, "nwc relay Start() failed: %s — leaving NWC disabled",
             esp_err_to_name(err));
    ctx.nwc_client.reset();
    ctx.nwc_subs.reset();
    ctx.nwc_relay.reset();
    return;
  }
  ctx.nwc_client->Start();
  ctx.nwc_enabled.store(true);
  ESP_LOGI(kTag, "nwc up: relay=%s wallet_pubkey=%.8s… refresh=%us flash=%d",
           url.c_str(), cfg.parsed.wallet_pubkey_hex.c_str(),
           static_cast<unsigned>(cfg.refresh_secs),
           cfg.flash_on_payment ? 1 : 0);
}

void RefreshNwcSettings(AppCtx& ctx) {
  settings::NvsPrefs prefs(prefs::kSettingsNs);
  const auto cfg = settings::ReadNwcConfig(prefs);
  ctx.nwc_flash_on_payment_enabled.store(cfg.flash_on_payment);
  ctx.nwc_notify_auto_restore.store(ctx.zap_screen_auto_restore.load());

  if (!ctx.nwc_client) {
    ESP_LOGI(kTag,
             "RefreshNwcSettings: client not wired, flash=%d auto_restore=%d "
             "(master toggle requires reboot)",
             cfg.flash_on_payment ? 1 : 0,
             ctx.zap_screen_auto_restore.load() ? 1 : 0);
    return;
  }
  // Re-prime the refresh timer if the cadence changed. Tear down +
  // reconstruct unconditionally — cheap, and avoids the branchy
  // "was-it-running" book-keeping that comes with esp_timer_restart.
  StopRefreshTimer(ctx);
  ctx.nwc_refresh_timer = StartRefreshTimer(ctx, cfg.refresh_secs);
  ESP_LOGI(kTag, "RefreshNwcSettings: refresh=%us flash=%d (timer %s)",
           static_cast<unsigned>(cfg.refresh_secs),
           cfg.flash_on_payment ? 1 : 0,
           ctx.nwc_refresh_timer ? "rearmed" : "off");
}

}  // namespace btclock
