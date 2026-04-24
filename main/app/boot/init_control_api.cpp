#include "app/boot/init_control_api.hpp"

#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/adapters.hpp"
#include "app/boot/factory_reset.hpp"
#include "app/boot/helpers.hpp"
#include "app/catalogs.hpp"
#include "io/light_sensor.hpp"
#include "io/mining_pool_selector.hpp"
#include "app/screen_manager.hpp"
#include "app/screen_slot_map.hpp"
#include "board/board.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "dnd/dnd.hpp"
#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nostr/relay_client.hpp"
#include "ota_manager.hpp"
#include "prefs.hpp"
#include "sdkconfig.h"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "sse_server.hpp"
#include "timezone/timezone.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";

// OTA asset naming mirrors the Arduino release workflow:
//   btclock_<variant>_ota.bin  / btclock_<variant>_webui.bin
// The variant slug matches the old `BOARD` define — lowercased with
// underscores. Rev A / Rev B / V8 are the only shipping variants
// today. Guarded behind POC_BOARD_* macros because the human-readable
// kHardwareName ("Rev B") contains a space.
OtaManager::Config MakeOtaConfig() {
  settings::NvsPrefs ota_prefs(prefs::kSettingsNs);
  OtaManager::Config ocfg;
  ocfg.release_url = ota_prefs.GetString(prefs::kGitReleaseUrl, "");
#if defined(POC_BOARD_REV_A)
  ocfg.firmware_asset = "btclock_rev_a_ota.bin";
  ocfg.webui_asset = "btclock_rev_a_webui.bin";
#elif defined(POC_BOARD_REV_B)
  ocfg.firmware_asset = "btclock_rev_b_ota.bin";
  ocfg.webui_asset = "btclock_rev_b_webui.bin";
#elif defined(POC_BOARD_V8)
  ocfg.firmware_asset = "btclock_v8_ota.bin";
  ocfg.webui_asset = "btclock_v8_webui.bin";
#else
  // TODO: fill in exact filename once a release ships for this variant.
  ocfg.firmware_asset = "btclock_unknown_ota.bin";
  ocfg.webui_asset = "btclock_unknown_webui.bin";
#endif
  return ocfg;
}

}  // namespace

void InitControlApi(AppCtx& ctx) {
  // Adapters first — they must live as long as the ControlServer that
  // holds raw Iface* pointers to them. AppCtx owns both, and the
  // declaration order in AppCtx puts `ctrl` ahead of the adapter
  // members so destruction runs in the right order (ctrl first).
  if (ctx.frontlight) {
    ctx.fl_adapter =
        std::make_unique<FrontlightAdapter>(ctx.frontlight.get());
  }
  ctx.leds_adapter = std::make_unique<LedsAdapter>();
  ctx.dnd_adapter = std::make_unique<DndAdapter>();
  if (ctx.light_sensor) {
    ctx.light_sensor_adapter =
        std::make_unique<LightSensorAdapter>(ctx.light_sensor.get());
  }
  ctx.timer_adapter = std::make_unique<TimerAdapter>(*ctx.sm, MsNow);

  // Factory-reset trigger shared by the HTTP endpoint and the
  // all-buttons-held hardware combo. The button path was armed in
  // main() before this TU is called; here we install the same
  // trampoline as the on_factory_reset webserver callback.
  auto* ctx_ptr = &ctx;
  auto factory_reset_trigger = [ctx_ptr]() {
    DoFactoryReset(*ctx_ptr->sm, ctx_ptr->panels, AppCtx::fb_storage(),
                   ctx_ptr->fonts, ctx_ptr->hub.get());
  };
  if (ctx.buttons) {
    ctx.buttons->SetOnAllButtonsLongPress(factory_reset_trigger);
  }

  // --- HTTP control API (STA mode only) ---
  if (!ctx.wifi->is_ap_mode()) {
    ControlServer::Config ccfg;
    ccfg.wifi = ctx.wifi.get();
    ccfg.hub = ctx.hub.get();
    ccfg.currencies = ctx.currencies;
    ccfg.num_screens = board::kNumPanels;
    ccfg.hw_name = board::kHardwareName;
    // Populates /api/settings `availablePools` so the WebUI dropdown
    // matches the compiled-in pool list. Kept in sync with the factory
    // in mining_pool_selector.cpp.
    ccfg.available_pools = mining_pools::AvailablePoolNames();
    ccfg.frontlight = ctx.fl_adapter.get();
    ccfg.leds = ctx.leds_adapter.get();
    ccfg.dnd = ctx.dnd_adapter.get();
    ccfg.timer = ctx.timer_adapter.get();
    ccfg.light_sensor = ctx.light_sensor_adapter.get();
    ccfg.on_factory_reset = factory_reset_trigger;
    // tzString PATCH -> setenv("TZ", ...) + tzset() so the clock screen
    // follows the new zone without reboot. Old firmware applied the zone
    // inline inside the PATCH handler; we keep the split so the webserver
    // component stays independent of the timezone component.
    ccfg.on_tz_changed = [](const std::string& zone) {
      if (zone.empty()) return;
      (void)timezone::SetTimezoneByName(zone.c_str());
    };
    // invertedColor PATCH hook — flip the EPD polarity flag + mark the
    // screen dirty so the next frame paints a full-refresh with the new
    // polarity. The data-driven render path runs on the main task's
    // event loop; the dirty flag is plain scalar state so poking it
    // from the httpd worker is safe without a command-queue hop.
    ScreenManager* sm_ptr = ctx.sm.get();
    ccfg.on_inverted_color_changed = [sm_ptr](bool v) {
      EpdSetGlobalInverted(v);
      sm_ptr->MarkDirty();
    };
    // fontName PATCH hook — rebind the AppFonts role accessors and mark
    // the screen dirty so the next render paints with the new family.
    // AppFonts lives on the stack-owned AppCtx; capture by ctx_ptr so
    // the lambda's lifetime matches the ControlServer's.
    ccfg.on_font_changed = [ctx_ptr](const std::string& id) {
      ctx_ptr->fonts.SetFamily(ParseFontFamily(id));
      if (ctx_ptr->sm) ctx_ptr->sm->MarkDirty();
    };
    // Mirror freshly-PATCHed DND fields into the singleton so the LED
    // and frontlight suppressor predicates (`dnd::Instance().IsActive()`,
    // evaluated on every frame) switch over to the new window without a
    // reboot. Settings persist to the `settings` namespace via the PATCH
    // handler; the singleton reads/writes its own `dnd` namespace, so
    // this hook is also what keeps the two namespaces in sync at
    // runtime.
    ccfg.on_dnd_changed = [] {
      Prefs settings_ns(prefs::kSettingsNs);
      auto& d = dnd::Instance();
      d.SetTimeEnabled(
          settings_ns.GetBool(prefs::kDndTimeEnabled, false));
      d.SetTimeRange(
          static_cast<uint8_t>(
              settings_ns.GetU32(prefs::kDndStartHour, 22) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(prefs::kDndStartMin, 0) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(prefs::kDndEndHour, 7) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(prefs::kDndEndMin, 0) & 0xFFu));
    };
    // Runtime catalogues for GET /api/settings drop-downs. Copies of the
    // constexpr arrays in app/catalogs.hpp; the settings handler holds
    // std::vector<std::string> slots so we materialise the views here
    // rather than refactoring the settings API type.
    for (const auto& f : catalogs::kAvailableFonts) {
      ccfg.available_fonts.emplace_back(f);
    }
    for (const auto& c : catalogs::kAvailableCurrencies) {
      ccfg.available_currencies.emplace_back(c);
    }
    for (const auto& s : catalogs::kScreenKinds) {
      ccfg.screens_catalog.push_back({s.api_id, std::string(s.display_label)});
    }
    // Nostr zap-relay liveness — read on every /api/status so the WebUI's
    // connection badge tracks reality instead of the hardcoded-false we
    // used before the ZapListener was wired.
    if (nostr::RelayClient* zap_ptr = ctx.zap_relay.get()) {
      ccfg.nostr_connected = [zap_ptr]() { return zap_ptr->connected(); };
    }
    // Capability gate — lets /api/settings and /api/show/screen know that
    // the mining-pool earnings slot is useless on a solo pool (no per-user
    // payout to render; the screen would forever show "0 SATS"). Evaluated
    // per-request so switching pool via PATCH /api/settings takes effect on
    // the next GET. Read is a cheap cached NVS lookup.
    ccfg.screen_is_hidden = [](int api_id) -> bool {
      if (api_id != slot_map::kApiIdMiningPoolEarnings) return false;
      Prefs p(prefs::kSettingsNs);
      const std::string name = p.GetString(prefs::kMiningPoolName, "");
      return !mining_pools::PoolSupportsDailyEarnings(name);
    };
    // POST /api/show/screen?s=<api_id> and the `currentScreen` field in
    // /api/status both speak the settings-catalog id the WebUI persists —
    // not ScreenManager's dense slot index. Bridge the two with pure-logic
    // helpers (app/screen_slot_map.hpp) so the HTTP handler stays free of
    // ScreenManager internals. api_id_to_slot lands per-currency screens
    // on whichever currency is currently displayed, matching the old
    // firmware's "stay on my current currency when I pick a new screen"
    // UX.
    ccfg.api_id_to_slot = [sm_ptr](int api_id) -> int {
      const auto& ccs = sm_ptr->currencies();
      std::size_t pref = 0;
      const std::string& cur = sm_ptr->current_currency();
      for (std::size_t i = 0; i < ccs.size(); ++i) {
        if (ccs[i] == cur) { pref = i; break; }
      }
      return slot_map::SlotForApiId(api_id, ccs.size(), pref);
    };
    ccfg.slot_to_api_id = [sm_ptr](std::size_t slot) -> int {
      return slot_map::ApiIdForSlot(slot, sm_ptr->currencies().size());
    };

    ctx.ctrl = std::make_unique<ControlServer>(std::move(ccfg));
    // SSE lifecycle: construct before Start() so RegisterRoute fires
    // in the same handler-registration pass as /api/*. The SseServer
    // does not own the httpd; ControlServer does, and stops it in its
    // destructor — which outlives `sse` via the destruction order of
    // these unique_ptrs.
    ctx.sse = std::make_unique<SseServer>(SseServer::Config{});
    ctx.ctrl->AttachSse(ctx.sse.get());
    if (ctx.ctrl->Start() != ESP_OK) {
      ESP_LOGE(kTag, "control server failed to start; control API disabled");
      ctx.ctrl.reset();
      ctx.sse.reset();
    }
  }

  // OTA manager — stores release URL + per-variant asset name so the
  // /api/firmware/auto_update handler can kick off a pull update.
  GetOtaManager().Init(MakeOtaConfig());

  // Re-hook DataHub on-update so fresh snapshots also fan out to SSE
  // subscribers. Capturing `ctrl`'s raw pointer is safe — the hub's
  // callback lifetime is bounded by this scope (no dangling after
  // app_main returns; app_main never returns in this firmware). We
  // keep the main-task notify so the render loop still wakes.
  if (ctx.hub) {
    ControlServer* ctrl_ptr = ctx.ctrl.get();
    TaskHandle_t main_task = ctx.main_task;
    ctx.hub->SetOnUpdate([main_task, ctrl_ptr](const DataSnapshot&) {
      xTaskNotifyGive(main_task);
      if (ctrl_ptr) ctrl_ptr->BroadcastStatus();
    });
  }

  // Seed the cached LiveStatus so the first /api/status after boot
  // reflects reality rather than a zeroed snapshot.
  PublishStatus(ctx);
}

void PublishStatus(AppCtx& ctx) {
  if (!ctx.ctrl || !ctx.sm) return;
  ControlServer::LiveStatus ls;
  ls.current_slot = static_cast<int32_t>(ctx.sm->current_slot());
  ls.slot_count = static_cast<int32_t>(ctx.sm->slot_count_public());
  ls.timer_running = !ctx.sm->IsPaused();
  ls.currency = ctx.sm->current_currency();
  ls.panel_texts = ctx.sm->last_panel_texts();
  ctx.ctrl->PublishStatus(ls);
  // Fan out to SSE subscribers so screen rotations / button presses
  // surface in the WebUI without waiting for the next poll.
  ctx.ctrl->BroadcastStatus();
}

}  // namespace btclock
