#include "app/boot/init_control_api.hpp"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/boot/adapters.hpp"
#include "app/boot/factory_reset.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_zap_listener.hpp"
#include "app/catalogs.hpp"
#include "app/rotation_plan.hpp"
#include "app/screen_manager.hpp"
#include "app/screen_slot_map.hpp"
#include "board/board.hpp"
#include "btclock_data.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "dnd/dnd.hpp"
#include "epd_ssd1680.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "io/light_sensor.hpp"
#include "io/mining_pool_selector.hpp"
#include "nostr/relay_client.hpp"
#include "nostr/zap_listener.hpp"
#include "ota_manager.hpp"
#include "ota_progress.hpp"
#include "prefs.hpp"
#include "screens/screens.hpp"
#include "sdkconfig.h"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "sources/mempool_kraken_source.hpp"
#include "sse_server.hpp"
#include "timezone/timezone.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";

// OTA asset naming mirrors the Arduino release workflow:
//   btclock_<board>[_<panel>]_ota.bin  / btclock_<board>[_<panel>]_webui.bin
// Board and panel slugs compose independently. The panel suffix is
// elided when the panel is the default (2.13") so the common case
// stays plain `btclock_rev_a_ota.bin` / `btclock_rev_b_ota.bin` / etc.
// — this matches the Arduino-era release artifact names. Untested
// non-default panels (e.g. rev_b_75) get an explicit suffix.
OtaManager::Config MakeOtaConfig() {
  settings::NvsPrefs ota_prefs(prefs::kSettingsNs);
  OtaManager::Config ocfg;
  ocfg.release_url =
      btclock::settings::ReadString(ota_prefs, prefs::kGitReleaseUrl);
#if defined(BTCLOCK_BOARD_REV_A)
#if defined(BTCLOCK_PANEL_2_9)
  ocfg.firmware_asset = "btclock_rev_a_29_ota.bin";
  ocfg.webui_asset = "btclock_rev_a_29_webui.bin";
#elif defined(BTCLOCK_PANEL_7_5)
  ocfg.firmware_asset = "btclock_rev_a_75_ota.bin";
  ocfg.webui_asset = "btclock_rev_a_75_webui.bin";
#else
  ocfg.firmware_asset = "btclock_rev_a_ota.bin";
  ocfg.webui_asset = "btclock_rev_a_webui.bin";
#endif
#elif defined(BTCLOCK_BOARD_REV_B)
#if defined(BTCLOCK_PANEL_2_9)
  ocfg.firmware_asset = "btclock_rev_b_29_ota.bin";
  ocfg.webui_asset = "btclock_rev_b_29_webui.bin";
#elif defined(BTCLOCK_PANEL_7_5)
  ocfg.firmware_asset = "btclock_rev_b_75_ota.bin";
  ocfg.webui_asset = "btclock_rev_b_75_webui.bin";
#else
  ocfg.firmware_asset = "btclock_rev_b_ota.bin";
  ocfg.webui_asset = "btclock_rev_b_webui.bin";
#endif
#elif defined(BTCLOCK_BOARD_V8)
#if defined(BTCLOCK_PANEL_2_9)
  ocfg.firmware_asset = "btclock_v8_29_ota.bin";
  ocfg.webui_asset = "btclock_v8_29_webui.bin";
#elif defined(BTCLOCK_PANEL_7_5)
  ocfg.firmware_asset = "btclock_v8_75_ota.bin";
  ocfg.webui_asset = "btclock_v8_75_webui.bin";
#else
  ocfg.firmware_asset = "btclock_v8_ota.bin";
  ocfg.webui_asset = "btclock_v8_webui.bin";
#endif
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
    ctx.fl_adapter = std::make_unique<FrontlightAdapter>(ctx.frontlight.get());
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
    // screen dirty + kick the main task so the next frame paints a
    // full-refresh with the new polarity. MarkDirty alone isn't enough:
    // event_loop's render path is gated on `got != 0` (a fresh
    // DataHub notification), so without a data push the dirty flag
    // would sit unprocessed until the next tick. xTaskNotifyGive here
    // wakes the event loop on the next iteration; the ShouldRender
    // check sees dirty_=true and re-renders with the new polarity
    // inside a single frame.
    ScreenManager* sm_ptr = ctx.sm.get();
    TaskHandle_t main_task_for_hooks = ctx.main_task;
    ccfg.on_inverted_color_changed = [sm_ptr, main_task_for_hooks](bool v) {
      EpdSetGlobalInverted(v);
      sm_ptr->MarkDirty();
      if (main_task_for_hooks) xTaskNotifyGive(main_task_for_hooks);
    };
    // fontName PATCH hook — rebind the AppFonts role accessors and mark
    // the screen dirty so the next render paints with the new family.
    // AppFonts lives on the stack-owned AppCtx; capture by ctx_ptr so
    // the lambda's lifetime matches the ControlServer's.
    ccfg.on_font_changed = [ctx_ptr](const std::string& id) {
      ctx_ptr->fonts.SetFamily(ParseFontFamily(id));
      if (ctx_ptr->sm) ctx_ptr->sm->MarkDirty();
      if (ctx_ptr->main_task) xTaskNotifyGive(ctx_ptr->main_task);
    };
    // blockFlashColor PATCH hook — push the new colour into the LED
    // controller's runtime cache so the next block flash uses the
    // user-chosen colour without reboot. SetBlockFlashColor also
    // re-persists to NVS, which is redundant after ApplyPatch but is a
    // no-op cost (matches the /api/lights direct-write paths).
    ccfg.on_block_flash_color_changed = [](uint32_t rgb) {
      SetBlockFlashColor(rgb);
    };
    // blockFeeDec PATCH hook — switches the v2 WS client's fee-stream
    // subscription between "blockfee" (integer) and "blockfee2"
    // (2-decimal) live. Stop+Start on the underlying WS so the relay
    // drops the previous topic; without that the client double-receives
    // and HandleBinaryFrame would pick whichever frame raced last.
    BtclockDataSource* ws_for_fee_dec = ctx_ptr->btclock_ws;
    ccfg.on_block_fee_dec_changed = [ws_for_fee_dec](bool v) {
      if (ws_for_fee_dec) ws_for_fee_dec->SetBlockFeeDec(v);
    };
    // ledBrightness / disableLeds / ledFlashOnUpd PATCH hooks — same
    // shape as blockFlashColor. Without these, PATCH would update the
    // settings NVS slot but the LED task's in-memory state (sampled
    // every frame) would keep the pre-PATCH value until reboot.
    ccfg.on_led_brightness_changed = [](uint8_t v) { SetLedBrightness(v); };
    ccfg.on_disable_leds_changed = [](bool v) { SetLedDisabled(v); };
    ccfg.on_led_flash_on_upd_changed = [](bool v) { SetLedFlashOnUpdate(v); };
    // Frontlight PATCH forwarder — re-reads every runtime-editable
    // frontlight pref and pushes the values into FrontlightController.
    // Mirrors the boot-time read in init_hardware.cpp so the two paths
    // can never drift. Only wired on boards with a PCA9685 backlight;
    // Rev A / V8 leave the hook null and the dispatcher in
    // control_server skips it. Brightness goes through SetBrightness
    // (queue-backed fade); the policy / gate setters are plain stores
    // and safe to call from the httpd worker. bd btclock_v4-7xv /
    // btclock_v4-63p.
    if (FrontlightController* fl = ctx.frontlight.get()) {
      ccfg.on_frontlight_changed = [fl] {
        Prefs settings(prefs::kSettingsNs);
        const uint32_t lux_threshold = settings.GetU32(
            prefs::kLuxLightToggle, frontlight::kDefaultLuxThreshold);
        fl->SetLuxThreshold(lux_threshold);
        fl->SetAmbientAutoOff(lux_threshold != 0);
        fl->SetOffWhenDark(
            btclock::settings::ReadBool(settings, prefs::kFlOffWhenDark));
        const uint32_t max_brightness =
            btclock::settings::ReadU32(settings, prefs::kFlMaxBrightness);
        if (max_brightness > 0 && max_brightness <= 0xFFFFu) {
          fl->SetConfiguredBrightness(static_cast<uint16_t>(max_brightness));
        }
        fl->SetEffectDelay(
            btclock::settings::ReadU32(settings, prefs::kFlEffectDelay));
        fl->SetAlwaysOn(
            btclock::settings::ReadBool(settings, prefs::kFlAlwaysOn));
        fl->SetDisabled(
            btclock::settings::ReadBool(settings, prefs::kFlDisable));
        fl->SetFlashOnUpdate(
            btclock::settings::ReadBool(settings, prefs::kFlFlashOnUpd));
      };
    }
    // Every successful /api/settings PATCH pulses the LEDs green so
    // the user sees an immediate confirmation the save landed. Piggy-
    // backs on the existing kFlashSuccess effect (3x green, 150ms) —
    // matches the old firmware's LED_FLASH_SUCCESS intent. Also marks
    // the screen dirty so display prefs (hideLeadZero, mowMode, etc.)
    // that ScreenManager samples from NVS per-render repaint on the
    // next tick instead of waiting for the next natural data change
    // (e.g. a minute rollover on the clock screen).
    ccfg.on_settings_patched = [sm_ptr, main_task_for_hooks] {
      PostLedEffect(LedEffect::kFlashSuccess);
      if (sm_ptr) sm_ptr->MarkDirty();
      if (main_task_for_hooks) xTaskNotifyGive(main_task_for_hooks);
    };
    // Rotation rebuild on screenOrder / screen<id>Visible / actCurrencies
    // PATCH. The sequence is otherwise built once at boot
    // (init_screen_manager.cpp) and runtime PATCHes wrote NVS but left the
    // live traversal stale, so the user had to reboot to see a reorder or
    // a currency-list change take effect. Mirrors the boot-time builder:
    // same `screenOrder` CSV + `screen<id>Visible` closure +
    // `actCurrencies` CSV. SetCurrencies first (slot_count depends on
    // currency count, so the new sequence must reflect the new count),
    // then SetRotationSequence to install the rebuilt plan. The data
    // source's currency subscription is refreshed in lock-step so price
    // ticks for newly-added codes start flowing without a reboot.
    BtclockDataSource* data_src = ctx_ptr->btclock_ws;
    ccfg.on_screens_changed = [ctx_ptr, sm_ptr, main_task_for_hooks, data_src] {
      if (!sm_ptr) return;
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
      sm_ptr->SetCurrencies(new_currencies);
      ctx_ptr->currencies = new_currencies;
      // The control server snapshot also needs the fresh list so
      // /api/show/currency stops 404'ing newly-added codes — the cfg
      // copy is otherwise a one-shot from boot.
      if (ctx_ptr->ctrl) ctx_ptr->ctrl->SetCurrencies(new_currencies);
      auto is_enabled = [](int api_id) -> bool {
        Prefs p(prefs::kSettingsNs);
        char vkey[24];
        std::snprintf(vkey, sizeof(vkey), "screen%dVisible", api_id);
        return p.GetBool(vkey, true);
      };
      sm_ptr->SetRotationSequence(rotation_plan::BuildRotationSequence(
          order_csv, is_enabled, sm_ptr->currencies().size()));
      // Refresh the v2 WS subscriptions so price frames start flowing
      // for codes the user just added. Stop+Start forces a fresh
      // subscribe set (additive `subscribe` frames alone wouldn't drop
      // a removed code's stream). The reconnect is ~5 s; mostly harmless
      // because the cached snapshot keeps the screens populated.
      if (data_src) data_src->SetCurrencies(new_currencies);
      if (main_task_for_hooks) xTaskNotifyGive(main_task_for_hooks);
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
          btclock::settings::ReadBool(settings_ns, prefs::kDndTimeEnabled));
      d.SetTimeRange(
          static_cast<uint8_t>(
              btclock::settings::ReadU32(settings_ns, prefs::kDndStartHour) &
              0xFFu),
          static_cast<uint8_t>(
              btclock::settings::ReadU32(settings_ns, prefs::kDndStartMin) &
              0xFFu),
          static_cast<uint8_t>(
              btclock::settings::ReadU32(settings_ns, prefs::kDndEndHour) &
              0xFFu),
          static_cast<uint8_t>(
              btclock::settings::ReadU32(settings_ns, prefs::kDndEndMin) &
              0xFFu));
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
    // dataSource=1 plumbs price/blocks straight from the source's
    // per-WS connection probes. dataSource=0 leaves all three callbacks
    // unset → control_server falls back to the hub-presence heuristic
    // (which is correct for v2 since the single socket carries both).
    // bd btclock_v4-1xc.
    if (MempoolKrakenSource* mk = ctx.mempool_kraken) {
      ccfg.price_connected = [mk]() { return mk->IsKrakenConnected(); };
      ccfg.blocks_connected = [mk]() { return mk->IsMempoolConnected(); };
      ccfg.v2_connected = []() { return false; };
    }
    // Live PATCH refresh for the runtime-editable nostr keys
    // (nostrZapPubkey, nostrZapNotify, ledFlashOnZap, flFlashOnZap,
    // scrnRestoreZap). The hook re-reads the canonical "settings"
    // namespace and rebuilds the listener subscription if the pubkey
    // changed; LED/frontlight/notify gates land via atomic refresh
    // alone. nostrRelay/nostrPubKey are boot_only and intentionally
    // skipped — the schema returns rebootRequired for those.
    // bd btclock_v4-aw5 / btclock_v4-q1l.
    ccfg.on_nostr_changed = [ctx_ptr] { RefreshZapListenerSettings(*ctx_ptr); };
    // POST /api/action/simulate_zap — drives the same code path the
    // real zap listener uses (DataSnapshot patch + pending flag +
    // task notify) but skips the LED / frontlight / nostrZapNotify
    // gates so the overlay fires unconditionally for QA.
    ccfg.simulate_zap = [ctx_ptr](int64_t amount_sats, std::string message) {
      if (!ctx_ptr->hub) return;
      DataSnapshot patch;
      patch.latest_zap.amount_sats = amount_sats;
      patch.latest_zap.message = std::move(message);
      patch.latest_zap.received_ms = MsNow();
      ctx_ptr->hub->Report(patch);
      ctx_ptr->zap_notify_pending.store(true);
      if (ctx_ptr->main_task) xTaskNotifyGive(ctx_ptr->main_task);
    };
    // Capability gate — lets /api/settings and /api/show/screen know that
    // the mining-pool earnings slot is useless on a solo pool (no per-user
    // payout to render; the screen would forever show "0 SATS"). Evaluated
    // per-request so switching pool via PATCH /api/settings takes effect on
    // the next GET. Read is a cheap cached NVS lookup.
    ccfg.screen_is_hidden = [](int api_id) -> bool {
      if (api_id != slot_map::kApiIdMiningPoolEarnings) return false;
      Prefs p(prefs::kSettingsNs);
      const std::string name =
          btclock::settings::ReadString(p, prefs::kMiningPoolName);
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
    // Completion-blink wiring for /upload/firmware — fires after the
    // 200 OK body has flushed and before ScheduleReboot. Kept inline so
    // the webserver component doesn't need to link against the LED
    // controller (it already depends on a stack of non-HW components).
    ccfg.on_ota_completion_blink = []() { PlayOtaCompletionBlink(3, 150); };

    ccfg.api_id_to_slot = [sm_ptr](int api_id) -> int {
      const auto& ccs = sm_ptr->currencies();
      std::size_t pref = 0;
      const std::string& cur = sm_ptr->current_currency();
      for (std::size_t i = 0; i < ccs.size(); ++i) {
        if (ccs[i] == cur) {
          pref = i;
          break;
        }
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

  // Pre-flash hook: fires once on the httpd worker before esp_ota_begin
  // erases the partition. We stop every data source (WS client, nostr
  // relay + zap listener, mining-pool + bitaxe pollers) to free the
  // internal heap their TLS / recv buffers were holding, then paint the
  // "UPDATE!" overlay on every panel and latch ScreenManager into OTA
  // mode so the main loop can't stomp it. The main render loop stops
  // painting once SetOtaOverlay(true) is observed — subsequent panel
  // writes come only from this handler's completion-blink branch (LED
  // only; the EPD stays on the "UPDATE!" screen until esp_restart).
  GetOtaManager().SetPreFlashHook([ctx_ptr]() {
    ESP_LOGW("ota-ux", "pre-flash hook: quiescing data + painting UPDATE");
    // Quiesce data sources — DataHub::StopAll is best-effort and safe
    // to call from any task.
    if (ctx_ptr->hub) ctx_ptr->hub->StopAll();
    // Nostr stack. Stop the listener first so its callback won't fire
    // after the relay closes; then stop the relay's own WS task.
    if (ctx_ptr->zap_listener) ctx_ptr->zap_listener->Stop();
    if (ctx_ptr->zap_relay) ctx_ptr->zap_relay->Stop();
    // Latch ScreenManager into OTA mode before painting — ShouldRender
    // now returns false so the main loop stays out of the EPD for the
    // remainder of the flash. Rotation timer is frozen via the same
    // predicate in MaybeAutoRotate.
    if (ctx_ptr->sm) {
      ctx_ptr->sm->SetOtaOverlay(true);
      RenderOtaUpdateScreen(ctx_ptr->panels, AppCtx::fb_storage(),
                            ctx_ptr->fonts);
    }
    // Seed the LED bar so the user sees "something is happening"
    // before the first real progress event fires.
    ShowOtaProgressLedCount(1);
  });

  // Progress callback: translate (written,total) into a 4-LED bar.
  // Content-Length missing → indeterminate indicator. The write phase
  // emits one event per ~16 KiB, so this lambda fires ~100 times over
  // a 1.5 MiB image — cheap enough for the httpd worker thread.
  GetOtaManager().SetProgressCallback([](const OtaProgress& p) {
    switch (p.phase) {
      case OtaProgress::Phase::kStarting:
        ShowOtaProgressLedCount(1);
        break;
      case OtaProgress::Phase::kWriting: {
        if (p.total == 0) {
          ShowOtaProgressIndeterminate();
        } else {
          const float frac = ProgressFraction(p.written, p.total);
          ShowOtaProgressLedCount(ProgressFractionToLedCount(frac));
        }
        break;
      }
      case OtaProgress::Phase::kVerifying:
        ShowOtaProgressLedCount(4);
        break;
      case OtaProgress::Phase::kRebooting:
        // Light all 4 pixels solid green. The blocking 3x completion
        // blink runs in HandleUploadFirmware after SendJson, so the
        // HTTP response flushes before we hold the httpd worker for
        // the ~1 s blink sequence.
        ShowOtaProgressLedCount(4);
        break;
      case OtaProgress::Phase::kFailed:
        // Clear the bar so a failed attempt leaves the strip dark,
        // then let the caller fall back to the normal resting state
        // via kSetIdle on the next interaction.
        ShowOtaProgressLedCount(0);
        break;
    }
  });

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
