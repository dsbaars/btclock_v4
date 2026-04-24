#include "app/boot/init_screen_manager.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "fonts_app.hpp"
#include "io/mining_pool_selector.hpp"
#include "app/screen_manager.hpp"
#include "buttons.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {

void InitScreenManager(AppCtx& ctx) {
  ctx.main_task = xTaskGetCurrentTaskHandle();
  ctx.button_q = xQueueCreate(8, sizeof(ButtonInput));
  assert(ctx.button_q != nullptr);

  // Bind the font-role accessors to the NVS-selected family before the
  // first render so screens that already use fonts.digit()/label()/etc.
  // paint with the user's pick on the very first frame rather than
  // flashing Antonio for one cycle. Unknown / empty values fall back to
  // Antonio via ParseFontFamily.
  {
    Prefs settings(prefs::kSettingsNs);
    const std::string id = settings.GetString(prefs::kFontName, "antonio");
    ctx.fonts.SetFamily(ParseFontFamily(id));
  }

  // Active currency set. For now hardcoded; beads lx0.11+ tracks the
  // NVS-backed config. Antonio's subset covers $/£/¥/€ symbols.
  ctx.currencies = {"USD", "EUR", "GBP", "JPY"};
  ctx.sm = std::make_unique<ScreenManager>(MsNow(), ctx.currencies);

  // Sats-symbol glyph index (0..15) lives in the "ui" namespace so it
  // can sit alongside future user-facing display prefs (color, layout)
  // rather than cohabiting with network / Nostr creds. ClampSatsVariant
  // defends against an out-of-range stored value — see fonts_app.hpp.
  {
    Prefs ui_prefs("ui");
    ctx.sm->SetSatsVariant(ClampSatsVariant(
        ui_prefs.GetU32("sats_variant", kSatsVariantDefault)));
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
        p.GetString(prefs::kMiningPoolName, "");
    return !mining_pools::PoolSupportsDailyEarnings(name);
  });
}

}  // namespace btclock
