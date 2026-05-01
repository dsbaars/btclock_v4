#include "app/boot/init_screen_manager.hpp"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
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
      char vkey[24];
      std::snprintf(vkey, sizeof(vkey), "screen%dVisible", api_id);
      return p.GetBool(vkey, true);
    };
    ctx.sm->SetRotationSequence(rotation_plan::BuildRotationSequence(
        order_csv, is_enabled, ctx.currencies.size()));
  }

  // Sats-symbol glyph index (0..15) lives in the "ui" namespace so it
  // can sit alongside future user-facing display prefs (color, layout)
  // rather than cohabiting with network / Nostr creds. ClampSatsVariant
  // defends against an out-of-range stored value — see fonts_app.hpp.
  {
    Prefs ui_prefs("ui");
    ctx.sm->SetSatsVariant(
        ClampSatsVariant(ui_prefs.GetU32("sats_variant", kSatsVariantDefault)));
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
