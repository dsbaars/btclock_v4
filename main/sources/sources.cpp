#include "sources/sources.hpp"

#include <memory>
#include <string>
#include <utility>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "io/led_controller.hpp"
#include "io/mining_pool_selector.hpp"
#include "app/screen_manager.hpp"
#include "bitaxe/bitaxe_source.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
#include "data_core/hub.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nostr/nostr_data_source.hpp"
#include "prefs.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";

void MaybeAddNostrSource(AppCtx& ctx) {
  // Optional Nostr DataSource. Opt-in via NVS (namespace "nostr"):
  //   key "enable" (bool, default false)  — master switch
  //   key "relay"  (string)                — wss:// URL
  //   key "pub"    (hex string, 64 chars)  — publisher pubkey
  // Missing or empty strings → skip cleanly rather than failing boot.
  // A future follow-up will expose these via the control-server /api
  // and the provisioning portal; for now set them with `nvs_tool` or a
  // one-shot boot-time Prefs::SetString().
  Prefs nostr_prefs("nostr");
  const bool enable = nostr_prefs.GetBool("enable", false);
  const std::string relay = nostr_prefs.GetString("relay", "");
  const std::string pub = nostr_prefs.GetString("pub", "");
  if (enable && !relay.empty() && !pub.empty()) {
    nostr::NostrDataSource::Config ncfg;
    ncfg.relay_url = relay;
    ncfg.author_pubkey_hex = pub;
    // Leave d_tags empty → subscribe to all slots the publisher
    // emits (price:*, blockheight, medianFee). Narrowing is a
    // future optimisation if the pubkey publishes more than we need.
    ctx.hub->AddSource(
        std::make_unique<nostr::NostrDataSource>(std::move(ncfg)));
    ESP_LOGI(kTag, "nostr enabled: relay=%s pub=%s…", relay.c_str(),
             pub.substr(0, 8).c_str());
  } else {
    ESP_LOGI(kTag, "nostr disabled (enable=%d relay=%s pub=%s)",
             enable ? 1 : 0, relay.empty() ? "<empty>" : "set",
             pub.empty() ? "<empty>" : "set");
  }
}

}  // namespace

void WireDataSources(AppCtx& ctx) {
  if (ctx.wifi->is_ap_mode()) return;

  ctx.hub = std::make_unique<DataHub>();
  ctx.hub->AddSource(std::make_unique<BtclockDataSource>(
      "wss://ws.btclock.dev/api/v2/ws", ctx.currencies));

  MaybeAddNostrSource(ctx);

  // Optional mining-pool HTTPS poller. Only the selected pool polls
  // (old-firmware behaviour — a single DataSnapshot::pool producer
  // avoids last-writer-wins races). Gated on settings/miningPoolStats;
  // disabled-by-default keeps Wi-Fi-bandwidth impact zero for users
  // who don't mine. See app/mining_pool_selector.{hpp,cpp}.
  if (auto pool_src = mining_pools::MakeActivePoolSource()) {
    ctx.hub->AddSource(std::move(pool_src));
  }

  // Optional Bitaxe LAN poller. Gated on settings/bitaxeEnabled +
  // a non-empty settings/bitaxeHostname. When both screens (hashrate
  // + best-diff) are in the rotation they paint "OFFLINE" until the
  // first poll lands.
  if (auto bitaxe_src = bitaxe::MakeBitaxeSource()) {
    ctx.hub->AddSource(std::move(bitaxe_src));
  }

  TaskHandle_t task = ctx.main_task;
  ctx.hub->SetOnUpdate([task](const DataSnapshot&) {
    xTaskNotifyGive(task);
  });
  ESP_ERROR_CHECK(ctx.hub->StartAll());

  // Block until the first blockheight arrives (or 30 s passes and we
  // paint whatever — the event loop will catch up when data lands).
  ESP_LOGI(kTag, "waiting for first blockheight push …");
  const int64_t deadline = MsNow() + 30'000;
  while (!ctx.hub->GetSnapshot().block_height) {
    const int64_t remain = deadline - MsNow();
    if (remain <= 0) break;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remain));
  }
  ctx.sm->Render(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                 ctx.hub->GetSnapshot());

  // Buttons come up after first paint so early clicks don't race a
  // blank display.
  ctx.buttons = std::make_unique<ButtonReader>(*ctx.mcp, ctx.button_q);
  ESP_ERROR_CHECK(ctx.buttons->Start());

  PostLedEvent(LedEvent::kSetIdle);
}

}  // namespace btclock
