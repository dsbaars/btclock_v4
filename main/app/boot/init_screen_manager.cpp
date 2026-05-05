#include "app/boot/init_screen_manager.hpp"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/catalogs.hpp"
#include "app/rotation_plan.hpp"
#include "app/screen_manager.hpp"
#include "buttons.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io/mining_pool_selector.hpp"
#include "pool_logo_fetcher/pool_logo_fetcher.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "screen_mgr_init";
}  // namespace

void InitScreenManager(AppCtx& ctx) {
  ctx.main_task = xTaskGetCurrentTaskHandle();
  ctx.button_q = xQueueCreate(8, sizeof(ButtonInput));
  // Production sdkconfig silences asserts, so check explicitly. The
  // button queue is load-bearing — every screen consumer reads it — so
  // failure here means the device is non-functional. Reboot rather
  // than silently spawning consumers that would xQueueReceive(nullptr);
  // a persistent OOM surfaces as a visible boot loop.
  if (ctx.button_q == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate failed for button queue; rebooting");
    vTaskDelay(pdMS_TO_TICKS(100));  // give the log line time to flush
    esp_restart();
  }

  // Bind the font-role accessors to the NVS-selected family before the
  // first render so screens that already use fonts.digit()/label()/etc.
  // paint with the user's pick on the very first frame rather than
  // flashing Antonio for one cycle. Unknown / empty values fall back to
  // Antonio via ParseFontFamily.
  {
    Prefs settings(prefs::kSettingsNs);
    const std::string id =
        btclock::settings::ReadString(settings, prefs::kFontName);
    ctx.fonts.SetFamily(ParseFontFamily(id));
  }

  // Active currency set lives under `actCurrencies` (CSV). Mirrors what
  // the settings API emits in GET /api/settings `actCurrencies[]` so the
  // rotation walks only the codes the user picked in the WebUI. Empty /
  // missing NVS value falls back to the WebUI's "first-launch defaults"
  // trio so a fresh device still rotates through something sensible.
  {
    Prefs settings(prefs::kSettingsNs);
    const std::string csv =
        btclock::settings::ReadString(settings, prefs::kActCurrencies);
    ctx.currencies.clear();
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) ctx.currencies.push_back(item);
    }
    if (ctx.currencies.empty()) ctx.currencies.push_back("USD");
  }
  // Seed the upstream-currency catalogue from the compile-time list. On
  // dataSource=0/2, WireDataSources replaces this with the live
  // `/api/v2/currencies` response; on dataSource=1 (mempool+kraken) it
  // stays as the catalogue. Kept in AppCtx instead of recomputed at the
  // settings_api layer so a single fetch round-trips into both the
  // /api/settings drop-down and the actCurrencies-filter set.
  ctx.available_currencies.clear();
  ctx.available_currencies.reserve(catalogs::kAvailableCurrencies.size());
  for (const auto& c : catalogs::kAvailableCurrencies) {
    ctx.available_currencies.emplace_back(c);
  }
  ctx.sm = std::make_unique<ScreenManager>(MsNow(), ctx.currencies);

  // Build the auto-rotate traversal sequence from the persisted
  // `screenOrder` CSV + `screen<id>Visible` toggles. When `screenOrder`
  // is unset (fresh device), the builder falls back to slot-index order
  // — pre-screenOrder default. `screenXVisible` defaults to true so a
  // missing key doesn't silently drop the screen from rotation.
  {
    Prefs settings(prefs::kSettingsNs);
    const std::string order_csv =
        btclock::settings::ReadString(settings, prefs::kScreenOrder);
    auto is_enabled = [](int api_id) -> bool {
      Prefs p(prefs::kSettingsNs);
      // Parent-feature gates: hashrate / earnings (70/71) live behind
      // miningPoolStats; bitaxe hashrate / best-diff (80/81) behind
      // bitaxeEnabled. Both default false in schema, so a fresh device
      // suppresses these rotation slots even though `screen<id>Visible`
      // defaults to true.
      if ((api_id == 70 || api_id == 71) &&
          !btclock::settings::ReadBool(p, prefs::kMiningPoolStats)) {
        return false;
      }
      if ((api_id == 80 || api_id == 81) &&
          !btclock::settings::ReadBool(p, prefs::kBitaxeEnabled)) {
        return false;
      }
      char vkey[24];
      std::snprintf(vkey, sizeof(vkey), "screen%dVisible", api_id);
      return p.GetBool(vkey, true);
    };
    ctx.sm->SetRotationSequence(rotation_plan::BuildRotationSequence(
        order_csv, is_enabled, ctx.currencies.size()));
  }

  // Resume cursor: by default the constructor leaves slot_=0
  // (block-height), but that ignores screenOrder — a user who put
  // Time first would always boot on block-height. The decision logic
  // (sentinel handling, in-sequence check, fee-rate trailing slot,
  // empty-sequence fallback) lives in `rotation_plan::ResumeSlot` so
  // host tests can exercise the same branches the firmware runs.
  // SetSlot() syncs rotation_idx_ via IndexForSlot so the next
  // auto-rotate step advances from the resumed position rather
  // than from a stale slot 0.
  {
    Prefs rt(prefs::kRuntimeStateNs);
    const uint32_t saved =
        rt.GetU32(prefs::kLastSlot, rotation_plan::kNoSavedSlot);
    const auto resume = rotation_plan::ResumeSlot(
        saved, ctx.sm->slot_count_public(), ctx.sm->rotation_sequence());
    if (resume) ctx.sm->SetSlot(*resume, MsNow());
    // Seed the block-height tracker from persisted runtime state so
    // the very first WS frame after a reboot can detect a missed
    // block and trigger the proper LED + frontlight + steal-focus
    // reaction (gated by IsCatchUpJump for big offline windows).
    // Without this seed, the ConsumeNewBlock debounce on `prev != 0`
    // silently swallows the first frame's update event. Mirrors
    // v3 commit 989e645 ("fix: Fix block number caching").
    const uint32_t saved_height = rt.GetU32(prefs::kLastBlockHeight, 0);
    if (saved_height != 0) ctx.sm->SeedLastSeenHeight(saved_height);
  }

  // Sats-symbol glyph index (0..15) — index into the 16 glyphs at
  // U+E000..U+E00F of the SatoshiSymbol font. Now lives in the
  // "settings" NVS namespace under `satsVariant` (camelCase, matches
  // the schema convention) so PATCH /api/settings can change it
  // live. Legacy fallback: devices that wrote the value under the
  // pre-schema "ui"/"sats_variant" key still pick it up — first-boot
  // schema default would otherwise revert their pick to 7 silently.
  // ClampSatsVariant guards both reads against an out-of-range
  // stored value (see main/fonts_app.hpp).
  {
    Prefs settings(prefs::kSettingsNs);
    const uint32_t schema_default = static_cast<uint32_t>(
        btclock::settings::DefaultIntFor(prefs::kSatsVariant));
    // Sentinel-on-absent: if "settings"/satsVariant is unset, fall
    // back to the legacy "ui"/sats_variant value (which itself defaults
    // to schema_default if absent).
    const uint32_t kSentinel = UINT32_MAX;
    const uint32_t from_settings =
        settings.GetU32(prefs::kSatsVariant, kSentinel);
    uint32_t resolved = schema_default;
    if (from_settings != kSentinel) {
      resolved = from_settings;
    } else {
      Prefs legacy("ui");
      resolved = legacy.GetU32("sats_variant", schema_default);
    }
    ctx.sm->SetSatsVariant(ClampSatsVariant(resolved));
  }
  // Rotation skip hook: honours the pool capability flag so solo pools
  // don't cycle onto the earnings slot even if the user's persisted
  // `screen71Visible` is still true. Evaluated per-tick so a PATCH that
  // flips miningPoolName picks up on the next rotation step without a
  // reboot. NVS GetString is cached after the first open — the hit is
  // effectively free.
  ctx.sm->SetSkipPredicate([](ScreenType kind) -> bool {
    if (kind != ScreenType::kMiningPoolEarnings) return false;
    Prefs p(prefs::kSettingsNs);
    const std::string name =
        btclock::settings::ReadString(p, prefs::kMiningPoolName);
    return !mining_pools::PoolSupportsDailyEarnings(name);
  });

  // Force a repaint when a logo fetch lands so the pool screens
  // displace the text-fallback that was painted while the bytes were
  // in flight. The fetcher invokes this from its own task; capture
  // the bare ScreenManager pointer (its lifetime is the app's).
  ScreenManager* sm_ptr = ctx.sm.get();
  btclock::pool_logos::SetOnFetchComplete(
      [sm_ptr](const std::string& /*pool_name*/) {
        if (sm_ptr) sm_ptr->MarkDirty();
      });
}

}  // namespace btclock
