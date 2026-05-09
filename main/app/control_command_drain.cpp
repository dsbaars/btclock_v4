#include "app/control_command_drain.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/rotation_plan.hpp"
#include "app/screen_manager.hpp"
#include "btclock_data.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/led_controller.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "timezone/timezone.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

bool DrainControlCommands(AppCtx& ctx) {
  ControlServer* ctrl = ctx.ctrl.get();
  if (!ctrl) return false;

  ScreenManager& sm = *ctx.sm;
  auto& hub = ctx.hub;

  ControlCommand ccmd{};
  bool re_render = false;
  bool any_cmd = false;
  while (ctrl->TryPopCommand(&ccmd)) {
    any_cmd = true;
    using Kind = ControlCommand::Kind;
    switch (ccmd.kind) {
      case Kind::kFullRefresh:
        sm.MarkDirty();
        re_render = true;
        break;
      case Kind::kIdentify:
        // Triple rapid multi-colour flash — matches the old firmware's
        // LED_FLASH_IDENTIFY (red↔cyan then green↔blue).
        PostLedEffect(LedEffect::kIdentify);
        break;
      case Kind::kRestart:
        ESP_LOGW(kTag, "restart requested via /api/restart");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        break;
      case Kind::kShowScreen:
        sm.SetSlot(static_cast<size_t>(ccmd.arg_i), MsNow());
        re_render = true;
        break;
      case Kind::kShowCurrency:
        sm.SetCurrency(ccmd.arg_s, MsNow());
        re_render = true;
        break;
      case Kind::kNextScreen:
        sm.NextScreen(MsNow());
        re_render = true;
        break;
      case Kind::kPrevScreen:
        sm.PrevScreen(MsNow());
        re_render = true;
        break;
      case Kind::kStopDataSources:
        if (hub) hub->StopAll();
        break;
      case Kind::kRestartDataSources:
        // StartAll() on an already-running source is a no-op for the
        // btclock WS source today; a clean stop+start is the closer
        // match to the old firmware. Cheap — sources only number 1.
        if (hub) {
          hub->StopAll();
          hub->StartAll();
        }
        break;
      case Kind::kShowCustom: {
        // Payload landed on ControlServer::pending_custom_ alongside
        // the command; pull it here before touching ScreenManager so
        // a concurrent second request can't slip its payload between
        // the pull and the apply.
        std::vector<std::string> cells;
        if (ctrl->TakePendingCustomCells(&cells)) {
          sm.SetCustomCells(std::move(cells), MsNow());
          re_render = true;
        }
        break;
      }
      case Kind::kSetBlockFeeDec: {
        // Mirror of kRebuildScreens for the blockFeeDec PATCH path.
        // BtclockDataSource::SetBlockFeeDec does Stop+Start on the
        // WS client; owning that on the main task keeps the data-
        // source lifecycle single-threaded.
        if (ctx.btclock_ws) ctx.btclock_ws->SetBlockFeeDec(ccmd.arg_i != 0);
        break;
      }
      case Kind::kSetFont: {
        // Re-read fontName from NVS and rebind AppFonts roles on the
        // main task so the four-pointer swap can't tear a render
        // frame across two families.
        Prefs settings(prefs::kSettingsNs);
        const std::string id =
            btclock::settings::ReadString(settings, prefs::kFontName);
        ctx.fonts.SetFamily(ParseFontFamily(id));
        sm.MarkDirty();
        re_render = true;
        break;
      }
      case Kind::kSetTimezone: {
        // Re-read tzString from NVS and apply via setenv+tzset on the
        // main task. localtime_r reads tz globals from the same task
        // each Render; deferring here closes the libc-internal race.
        Prefs settings(prefs::kSettingsNs);
        const std::string zone =
            btclock::settings::ReadString(settings, prefs::kTzString);
        if (!zone.empty()) (void)timezone::SetTimezoneByName(zone.c_str());
        sm.MarkDirty();
        re_render = true;
        break;
      }
      case Kind::kPublishLiveStatus:
        // No ScreenManager mutation — fan-out cached LiveStatus + framebuffer
        // preview snapshot from the main task only (WS handler runs on httpd).
        break;
      case Kind::kRebuildScreens: {
        // Settings PATCH that changed actCurrencies / screenOrder /
        // screen<id>Visible. Re-read NVS here (main task) so all
        // ScreenManager + BtclockDataSource mutations stay
        // single-threaded — see init_control_api.cpp's
        // on_screens_changed for why this is deferred.
        Prefs settings(prefs::kSettingsNs);
        const std::string order_csv =
            btclock::settings::ReadString(settings, prefs::kScreenOrder);
        const std::string ccy_csv =
            btclock::settings::ReadString(settings, prefs::kActCurrencies);
        std::vector<std::string> new_currencies;
        {
          std::stringstream ss(ccy_csv);
          std::string item;
          while (std::getline(ss, item, ',')) {
            if (!item.empty()) new_currencies.push_back(item);
          }
          if (new_currencies.empty()) new_currencies.push_back("USD");
        }
        // SetCurrencies first — slot_count depends on the new currency
        // count, so the rotation sequence must reflect the new size.
        sm.SetCurrencies(new_currencies);
        ctx.currencies = new_currencies;
        // Mirror to the control-server snapshot so /api/show/currency
        // recognises newly-added codes. Same task as the HTTP handlers
        // that read it (httpd worker thread vs main is irrelevant —
        // the read path takes its own lock for `status_`; `cfg_` is
        // updated atomically here and reads of cfg_.currencies are
        // tolerant of stale-but-consistent reads).
        ctrl->SetCurrencies(new_currencies);
        auto is_enabled = [](int api_id) -> bool {
          Prefs p(prefs::kSettingsNs);
          // Parent-feature gates: keep the runtime rebuild in sync with
          // boot-time init_screen_manager. Without these checks a
          // PATCH that flips miningPoolStats / bitaxeEnabled wouldn't
          // drop the disabled slots from rotation until reboot.
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
        sm.SetRotationSequence(rotation_plan::BuildRotationSequence(
            order_csv, is_enabled, sm.currencies().size()));
        // Refresh v2 WS subscriptions so price frames flow for newly-
        // added codes. Stop+Start forces a fresh subscribe set.
        if (ctx.btclock_ws) ctx.btclock_ws->SetCurrencies(new_currencies);
        re_render = true;
        break;
      }
    }
  }

  if (!any_cmd) return false;

  if (re_render && hub) {
    sm.Render(ctx.panels, AppCtx::fb_storage(), ctx.fonts, hub->GetSnapshot());
  }
  PublishStatus(ctx);
  return true;
}

}  // namespace btclock
