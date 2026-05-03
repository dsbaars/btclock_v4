#include "sources/sources.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/screen_manager.hpp"
#include "bitaxe/bitaxe_source.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
#include "data_core/hub.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/led_controller.hpp"
#include "io/mining_pool_selector.hpp"
#include "nostr/nostr_data_source.hpp"
#include "prefs.hpp"
#include "settings/nostr_config.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "sources/mempool_kraken_source.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";

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
  // Reject bare hostnames / https:// values up front. The PATCH path
  // validates the scheme on the way in, but devices flashed before
  // that gate can carry a stale schemeless `nostrRelay` in NVS.
  // Constructing NostrDataSource with such a URL would fail at Start()
  // with "Invalid uri", and the StartAll() aggregate used to abort the
  // boot (Rev B reboot loop). Keep the source out of the hub entirely
  // so the rest of the data pipeline (block/price feeds) still comes up.
  const bool relay_scheme_ok = cfg.relay_url.rfind("wss://", 0) == 0 ||
                               cfg.relay_url.rfind("ws://", 0) == 0;
  if (cfg.enabled && !cfg.relay_url.empty() && !cfg.author_pubkey_hex.empty() &&
      relay_scheme_ok) {
    nostr::NostrDataSource::Config ncfg;
    ncfg.relay_url = cfg.relay_url;
    ncfg.author_pubkey_hex = cfg.author_pubkey_hex;
    // Leave d_tags empty → subscribe to all slots the publisher
    // emits (price:*, blockheight, medianFee). Narrowing is a
    // future optimisation if the pubkey publishes more than we need.
    auto source = std::make_unique<nostr::NostrDataSource>(std::move(ncfg));
    // Stash the raw pointer before the unique_ptr is moved into the hub,
    // so InitZapListener can consult subs() / relay_url() to share the
    // single WSS instead of opening a second one. Lifetime: the hub
    // outlives the listener (declaration order in AppCtx), so the
    // back-ref stays valid for the listener's whole life.
    ctx.nostr_source = source.get();
    ctx.hub->AddSource(std::move(source));
    ESP_LOGI(kTag, "nostr enabled: relay=%s pub=%s…", cfg.relay_url.c_str(),
             cfg.author_pubkey_hex.substr(0, 8).c_str());
  } else {
    ESP_LOGI(kTag, "nostr disabled (enable=%d relay=%s pub=%s scheme_ok=%d)",
             cfg.enabled ? 1 : 0, cfg.relay_url.empty() ? "<empty>" : "set",
             cfg.author_pubkey_hex.empty() ? "<empty>" : "set",
             relay_scheme_ok ? 1 : 0);
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
    // ReadU8 lives only on PrefsReader (used by settings_api); the bare
    // Prefs handle stores u8 keys as u32 anyway, so ReadU32 + truncate
    // produces the same byte the schema declares.
    data_source = static_cast<std::uint8_t>(
        btclock::settings::ReadU32(settings, prefs::kDataSource));
    // ceEndpoint is the custom-endpoint host used when dataSource=1.
    // dataSource=0 (the default) routes through BuildBtclockSourceUri
    // and ignores ceEndpoint entirely, so the schema default lands only
    // when the user actively picks the custom-endpoint mode.
    ce_endpoint = btclock::settings::ReadString(settings, prefs::kCeEndpoint);
    ce_disable_ssl =
        btclock::settings::ReadBool(settings, prefs::kCeDisableSSL);
    block_fee_dec = btclock::settings::ReadBool(settings, prefs::kBlockFeeDec);
  }
  if (data_source == 3) {
    ESP_LOGW(kTag, "dataSource=%d not implemented, using btclock_v2 fallback",
             static_cast<int>(data_source));
  }

  if (data_source == 1) {
    // mempool.space + Kraken — two independent WSS connections, neither
    // takes the other down. Skips the v2 source entirely; the
    // ctx.btclock_ws back-ref stays null which the actCurrencies and
    // blockFeeDec hooks check before calling SetCurrencies / SetBlockFeeDec
    // on it. A future hook for the mempool+kraken source will need its
    // own back-ref slot in AppCtx.
    ESP_LOGI(kTag,
             "dataSource=1 → mempool.space + Kraken (currencies=%u, "
             "block_fee_dec=%d)",
             static_cast<unsigned>(ctx.currencies.size()),
             block_fee_dec ? 1 : 0);
    auto mempool_kraken =
        std::make_unique<MempoolKrakenSource>(ctx.currencies, block_fee_dec);
    ctx.mempool_kraken = mempool_kraken.get();
    ctx.hub->AddSource(std::move(mempool_kraken));
  } else {
    const std::string uri =
        BuildBtclockSourceUri(data_source, ce_endpoint, ce_disable_ssl);
    ESP_LOGI(kTag, "btclock_ws connecting to: %s (dataSource=%d)", uri.c_str(),
             static_cast<int>(data_source));

    // Keep a non-owning back-ref to the v2 WS source so the
    // on_screens_changed hook in init_control_api can refresh its
    // currency subscription list when the user PATCHes actCurrencies —
    // without it the WS keeps streaming the old currencies until reboot.
    auto btclock_ws = std::make_unique<BtclockDataSource>(
        uri.c_str(), ctx.currencies, block_fee_dec);
    ctx.btclock_ws = btclock_ws.get();
    ctx.hub->AddSource(std::move(btclock_ws));
  }

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
  ctx.hub->SetOnUpdate([task](const DataSnapshot&) { xTaskNotifyGive(task); });
  // Don't ESP_ERROR_CHECK here — a single bad source (e.g. a malformed
  // nostrRelay URL persisted before scheme validation landed) returns
  // -1 from its Start() and rolls up into a non-OK aggregate. Aborting
  // the whole boot for that case bricks the device into a reboot loop
  // (the Rev B regression following bd btclock_v4-1xc). Sources that
  // fail to start log their own error; the hub keeps running with the
  // ones that came up cleanly, and /api/status's connection probes
  // surface the partial-failure state to the WebUI.
  if (auto err = ctx.hub->StartAll(); err != ESP_OK) {
    ESP_LOGW(kTag, "hub StartAll partial failure: %s — continuing",
             esp_err_to_name(err));
  }

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
  // inverseButtons swaps button-1↔button-N at post time so users with
  // an upside-down enclosure get nav/pause aligned with the physical
  // layout. Boot-only read; live PATCH requires a reboot per
  // SETTINGS.md.
  {
    Prefs settings_for_btn(prefs::kSettingsNs);
    ctx.buttons->SetInverted(
        btclock::settings::ReadBool(settings_for_btn, prefs::kInverseButtons));
  }
  ESP_ERROR_CHECK(ctx.buttons->Start());

  PostLedEvent(LedEvent::kSetIdle);
}

}  // namespace btclock
