#include "sources/sources.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/screen_manager.hpp"
#include "bitaxe/bitaxe_source.hpp"
#include "board/board.hpp"
#include "boot_spinner.hpp"
#include "btclock_currencies_fetch.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
#include "control_server.hpp"
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
#include "sources/bip110_source.hpp"
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
  // Schema keys: kDataSource (==2 selects Nostr), kNostrRelays (CSV
  // canonical / kNostrRelay singular fallback), kNostrPubKey. All four
  // are flagged boot_only in the schema so a change requires reboot —
  // no live-reload hook here.
  settings::NvsPrefs settings_prefs(prefs::kSettingsNs);
  const auto cfg = settings::ReadNostrSourceConfig(settings_prefs);
  if (!cfg.enabled || cfg.relay_urls.empty() || cfg.author_pubkey_hex.empty()) {
    ESP_LOGI(kTag, "nostr disabled (enable=%d relays=%u pub=%s)",
             cfg.enabled ? 1 : 0, static_cast<unsigned>(cfg.relay_urls.size()),
             cfg.author_pubkey_hex.empty() ? "<empty>" : "set");
    return;
  }
  // One NostrDataSource per relay. Hub::Report Merge is content-
  // idempotent and NostrDataSource keeps its own per-d staleness map,
  // so duplicate NIP-78 events from sibling relays collapse to a no-op
  // — no extra dedup needed on the data path. The zap path needs an
  // explicit event-id LRU; that lives in init_zap_listener.cpp.
  ctx.nostr_sources.reserve(cfg.relay_urls.size());
  for (const auto& url : cfg.relay_urls) {
    // Reject bare hostnames / https:// values up front. The PATCH path
    // validates the scheme on the way in, but devices flashed before
    // that gate can carry a stale schemeless `nostrRelay` in NVS.
    // Constructing NostrDataSource with such a URL would fail at
    // Start() with "Invalid uri", and the StartAll() aggregate used to
    // abort the boot (Rev B reboot loop). Skip the bad relay and keep
    // wiring the rest so a partially-corrupt list still gives a
    // usable boot.
    const bool scheme_ok =
        url.rfind("wss://", 0) == 0 || url.rfind("ws://", 0) == 0;
    if (!scheme_ok) {
      ESP_LOGW(kTag, "nostr relay skipped (bad scheme): %s", url.c_str());
      continue;
    }
    nostr::NostrDataSource::Config ncfg;
    ncfg.relay_url = url;
    ncfg.author_pubkey_hex = cfg.author_pubkey_hex;
    // Per-source NIP-01 sub identifier — unique per relay so the
    // SubscriptionManager dispatches CLOSE/EVENT to the right consumer
    // even though every NostrDataSource owns its own manager today.
    // Future-proof against accidental sub-id collisions if we ever
    // share managers across multiple data sources.
    ncfg.sub_id = "btclock-v1-" + std::to_string(ctx.nostr_sources.size());
    // Leave d_tags empty → subscribe to all slots the publisher emits
    // (price:*, blockheight, medianFee). Narrowing is a future
    // optimisation if the pubkey publishes more than we need.
    auto source = std::make_unique<nostr::NostrDataSource>(std::move(ncfg));
    // Stash the raw pointer before the unique_ptr is moved into the hub
    // so InitZapListener can consult subs() / relay_url() to share the
    // single WSS instead of opening a second one. Lifetime: the hub
    // outlives the listener (declaration order in AppCtx), so the
    // back-refs stay valid for the listeners' whole lives.
    ctx.nostr_sources.push_back(source.get());
    ctx.hub->AddSource(std::move(source));
    ESP_LOGI(kTag, "nostr enabled: relay=%s pub=%s…", url.c_str(),
             cfg.author_pubkey_hex.substr(0, 8).c_str());
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

    // The upstream `/api/v2/currencies` fetch + active-list prune is
    // deferred to the first STA connect (RefreshUpstreamCurrencies): boot
    // is non-blocking now, so there's no reachable upstream here, and a
    // blocking HTTPS GET would just stall boot. The source subscribes to
    // the user's configured currencies for now; RefreshUpstreamCurrencies
    // re-subscribes via BtclockDataSource::SetCurrencies if the upstream
    // catalogue prunes the list.

    // Keep a non-owning back-ref to the v2 WS source so the
    // on_screens_changed hook in init_control_api (and the deferred
    // currency refresh) can update its subscription list without poking
    // through DataHub internals.
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

  // BIP-110 chain-tip poller for the dual block-height screen. Gated on
  // that screen being visible (see MakeBip110Source) so a device that
  // never shows it doesn't poll a third-party mempool instance.
  if (auto bip110_src = bip110::MakeBip110Source()) {
    ctx.hub->AddSource(std::move(bip110_src));
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
}

void RefreshUpstreamCurrencies(AppCtx& ctx) {
  // Deferred companion to WireDataSources, run on the first STA connect:
  // fetch the upstream currency catalogue and prune the active rotation to
  // what the backend actually serves. Only the v2 paths (ds=0/2) have a
  // catalogue; ds=1 (mempool+kraken) and the no-source case are no-ops.
  if (!ctx.btclock_ws) return;

  std::uint8_t data_source = 0;
  std::string ce_endpoint;
  bool ce_disable_ssl = false;
  {
    Prefs settings(prefs::kSettingsNs);
    data_source = static_cast<std::uint8_t>(
        btclock::settings::ReadU32(settings, prefs::kDataSource));
    ce_endpoint = btclock::settings::ReadString(settings, prefs::kCeEndpoint);
    ce_disable_ssl =
        btclock::settings::ReadBool(settings, prefs::kCeDisableSSL);
  }
  if (data_source == 1) return;

  const std::string uri =
      BuildBtclockSourceUri(data_source, ce_endpoint, ce_disable_ssl);
  auto fetched = FetchAvailableCurrencies(uri);
  if (fetched.empty()) return;  // non-fatal: keep the seeded catalogue
  ctx.available_currencies = std::move(fetched);
  // Propagate the fetched catalogue to the control server so
  // /api/settings.availableCurrencies (the WebUI currency dropdown)
  // reflects the upstream set — BuildDeviceContext copies from the
  // control server's config, not the AppCtx vector, so without this the
  // dropdown stays stuck on the seeded subset.
  if (ctx.ctrl) ctx.ctrl->SetAvailableCurrencies(ctx.available_currencies);

  std::vector<std::string> filtered;
  filtered.reserve(ctx.currencies.size());
  for (const auto& code : ctx.currencies) {
    for (const auto& avail : ctx.available_currencies) {
      if (code == avail) {
        filtered.push_back(code);
        break;
      }
    }
  }
  if (filtered.empty() && !ctx.available_currencies.empty()) {
    filtered.push_back(ctx.available_currencies.front());
  }
  if (filtered != ctx.currencies) {
    ESP_LOGI(kTag,
             "actCurrencies pruned to upstream catalogue (was %u, now %u)",
             static_cast<unsigned>(ctx.currencies.size()),
             static_cast<unsigned>(filtered.size()));
    ctx.currencies = std::move(filtered);
    if (ctx.sm) ctx.sm->SetCurrencies(ctx.currencies);
    // The source already subscribed to the un-pruned list at boot, so
    // re-subscribe it to the pruned set (same path as the live actCurrencies
    // PATCH hook).
    ctx.btclock_ws->SetCurrencies(ctx.currencies);
  }
}

void FinishBoot(AppCtx& ctx) {
  // Pure-provisioning boot (empty creds): the portal UI owns the panels +
  // LEDs and there are no buttons. Nothing to finish.
  if (!ctx.sm || ctx.wifi->is_ap_mode()) return;

  // Non-blocking boot: the data hub is wired lazily on the first STA
  // connect (NetworkCoordinator), so it's typically null here. Drop the
  // boot spinner and paint a first frame — placeholders until the first
  // data arrives — so the device is visibly up. The old "wait up to 30 s
  // for the first blockheight" gate is gone: with a non-blocking connect
  // there may be no network at all, and waiting would just stall boot.
  StopBootSpinner();
  ctx.sm->Render(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                 ctx.hub ? ctx.hub->GetSnapshot() : DataSnapshot{});

  // Buttons come up after first paint so early clicks don't race a
  // blank display.
  ctx.buttons = std::make_unique<ButtonReader>(*ctx.mcp, ctx.button_q);
  // inverseButtons swaps button-1↔button-N at post time so users with
  // an upside-down enclosure get nav/pause aligned with the physical
  // layout. Boot-only read; live PATCH requires a reboot per
  // SETTINGS.md. XOR'd against the board's hardware-default orientation
  // because V8 wires its buttons opposite of Rev A/B (V8: MCP1 pin 0 ==
  // button 1; Rev A/B: pin 3 == button 1).
  {
    Prefs settings_for_btn(prefs::kSettingsNs);
    const bool pref =
        btclock::settings::ReadBool(settings_for_btn, prefs::kInverseButtons);
    ctx.buttons->SetInverted(board::kButtonsInvertedDefault ^ pref);
  }
  ESP_ERROR_CHECK(ctx.buttons->Start());

  PostLedEffect(LedEffect::kSetIdle);

  // This first render ran outside the event loop's render paths (each of
  // which publishes afterwards), so publish the live status now. Otherwise
  // /api/status.data[] keeps the empty placeholder InitControlApi cached
  // before any render until the next event-loop render — which, on a
  // static screen with no new block or rotation, may not come for a long
  // time (block height visible on the panels, data[] empty in the API).
  PublishStatus(ctx);
}

}  // namespace btclock
