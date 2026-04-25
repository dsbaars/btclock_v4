#include "sources/sources.hpp"

#include <cstdint>
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
#include "settings/nostr_config.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";

void MaybeAddNostrSource(AppCtx& ctx) {
  // Settings live in the canonical "settings" NVS namespace where
  // /api/settings PATCH writes them — readers used to open a separate
  // "nostr" namespace with shorthand keys ("enable" / "relay" / "pub")
  // so the WebUI's PATCH was effectively a no-op (bd btclock_v4-aw5).
  // Schema keys: kDataSource (==2 selects Nostr), kNostrRelay,
  // kNostrPubKey. All three are flagged boot_only in the schema so a
  // change requires reboot — no live-reload hook here.
  settings::NvsPrefs settings_prefs(prefs::kSettingsNs);
  const auto cfg = settings::ReadNostrSourceConfig(settings_prefs);
  if (cfg.enabled && !cfg.relay_url.empty() &&
      !cfg.author_pubkey_hex.empty()) {
    nostr::NostrDataSource::Config ncfg;
    ncfg.relay_url = cfg.relay_url;
    ncfg.author_pubkey_hex = cfg.author_pubkey_hex;
    // Leave d_tags empty → subscribe to all slots the publisher
    // emits (price:*, blockheight, medianFee). Narrowing is a
    // future optimisation if the pubkey publishes more than we need.
    ctx.hub->AddSource(
        std::make_unique<nostr::NostrDataSource>(std::move(ncfg)));
    ESP_LOGI(kTag, "nostr enabled: relay=%s pub=%s…",
             cfg.relay_url.c_str(),
             cfg.author_pubkey_hex.substr(0, 8).c_str());
  } else {
    ESP_LOGI(kTag, "nostr disabled (enable=%d relay=%s pub=%s)",
             cfg.enabled ? 1 : 0,
             cfg.relay_url.empty() ? "<empty>" : "set",
             cfg.author_pubkey_hex.empty() ? "<empty>" : "set");
  }
}

}  // namespace

void WireDataSources(AppCtx& ctx) {
  if (ctx.wifi->is_ap_mode()) return;

  ctx.hub = std::make_unique<DataHub>();

  // Resolve the WSS URI from settings before constructing the source.
  // dataSource / ceEndpoint / ceDisableSSL are all boot_only — schema
  // marks them as such — so a one-shot read at boot is correct.
  std::uint8_t data_source = 0;
  std::string ce_endpoint;
  bool ce_disable_ssl = false;
  bool block_fee_dec = true;
  {
    Prefs settings(prefs::kSettingsNs);
    data_source = static_cast<std::uint8_t>(
        settings.GetU32(prefs::kDataSource, 0));
    ce_endpoint = settings.GetString(prefs::kCeEndpoint, "");
    ce_disable_ssl = settings.GetBool(prefs::kCeDisableSSL, false);
    // Default true matches DEFAULT_BLOCK_FEE_DECIMALS in the v3 firmware
    // and the schema's default_bool=true on prefs::kBlockFeeDec.
    block_fee_dec = settings.GetBool(prefs::kBlockFeeDec, true);
  }
  if (data_source == 1 || data_source == 3) {
    ESP_LOGW(kTag,
             "dataSource=%d not implemented, using btclock_v2 fallback",
             static_cast<int>(data_source));
  }
  const std::string uri =
      BuildBtclockSourceUri(data_source, ce_endpoint, ce_disable_ssl);
  ESP_LOGI(kTag, "btclock_ws connecting to: %s (dataSource=%d)",
           uri.c_str(), static_cast<int>(data_source));

  // Keep a non-owning back-ref to the v2 WS source so the
  // on_screens_changed hook in init_control_api can refresh its
  // currency subscription list when the user PATCHes actCurrencies —
  // without it the WS keeps streaming the old currencies until reboot.
  auto btclock_ws = std::make_unique<BtclockDataSource>(
      uri.c_str(), ctx.currencies, block_fee_dec);
  ctx.btclock_ws = btclock_ws.get();
  ctx.hub->AddSource(std::move(btclock_ws));

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
