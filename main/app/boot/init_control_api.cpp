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
#include "app/boot/init_mdns.hpp"
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
#include "esp_system.h"
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
// Both slugs come from the top-level CMakeLists.txt as compile defs:
// BTCLOCK_BOARD_SLUG is the lowercase board name ("rev_a" / "rev_b" /
// "v8"), BTCLOCK_PANEL_ASSET_SUFFIX is "" for the default 2.13" panel
// and "_29" / "_75" for the non-default geometries. Adjacent string
// literals are concatenated by the preprocessor, so the synthesis is a
// pure compile-time fold — no #if cross-product, no runtime branching,
// no "unknown variant" branch to forget.
OtaManager::Config MakeOtaConfig() {
  settings::NvsPrefs ota_prefs(prefs::kSettingsNs);
  OtaManager::Config ocfg;
  ocfg.release_url =
      btclock::settings::ReadString(ota_prefs, prefs::kGitReleaseUrl);
  ocfg.firmware_asset =
      "btclock_" BTCLOCK_BOARD_SLUG BTCLOCK_PANEL_ASSET_SUFFIX "_ota.bin";
  ocfg.webui_asset =
      "btclock_" BTCLOCK_BOARD_SLUG BTCLOCK_PANEL_ASSET_SUFFIX "_webui.bin";
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
    ccfg.hw_id = board::kHardwareId;
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
    //
    // Deferred to main via the ControlCommand queue so tzset() can't
    // race localtime_r() on the render path (picolibc has no internal
    // lock between them). bd btclock_v4-flb.
    ccfg.on_tz_changed = [ctx_ptr](const std::string& zone) {
      if (!ctx_ptr || !ctx_ptr->ctrl) return;
      if (zone.empty()) return;
      ControlCommand cmd{ControlCommand::Kind::kSetTimezone};
      ctx_ptr->ctrl->PostCommand(cmd);
      if (ctx_ptr->main_task) xTaskNotifyGive(ctx_ptr->main_task);
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
    // Deferred to main via the ControlCommand queue so the four-pointer
    // role swap inside AppFonts::SetFamily is atomic with respect to
    // any in-flight Render (avoids a single-frame mixed-family glyph
    // mismatch). bd btclock_v4-5az.
    ccfg.on_font_changed = [ctx_ptr](const std::string& id) {
      (void)id;  // re-read from NVS in the main-task drain
      if (!ctx_ptr || !ctx_ptr->ctrl) return;
      ControlCommand cmd{ControlCommand::Kind::kSetFont};
      ctx_ptr->ctrl->PostCommand(cmd);
      if (ctx_ptr->main_task) xTaskNotifyGive(ctx_ptr->main_task);
    };
    // satsVariant PATCH hook — rebind ScreenManager's glyph index so
    // the next render of moscow_time / nostr_zap paints with the new
    // codepoint. ClampSatsVariant guards against an out-of-range
    // store; the schema already 0..15-clamps via FieldSpec range, so
    // this is belt-and-braces against future bypass paths.
    ccfg.on_sats_variant_changed = [ctx_ptr](uint8_t v) {
      if (ctx_ptr->sm) {
        ctx_ptr->sm->SetSatsVariant(ClampSatsVariant(v));
        ctx_ptr->sm->MarkDirty();
      }
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
    //
    // Defer to main via the ControlCommand queue (mirrors
    // kRebuildScreens — same rationale, BtclockDataSource ownership
    // belongs to the main task). bd btclock_v4-22e.
    ccfg.on_block_fee_dec_changed = [ctx_ptr, main_task_for_hooks](bool v) {
      if (!ctx_ptr || !ctx_ptr->ctrl) return;
      ControlCommand cmd{ControlCommand::Kind::kSetBlockFeeDec};
      cmd.arg_i = v ? 1 : 0;
      ctx_ptr->ctrl->PostCommand(cmd);
      if (main_task_for_hooks) xTaskNotifyGive(main_task_for_hooks);
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
    // a currency-list change take effect.
    //
    // Crucially this hook fires on the HTTP task, but ScreenManager and
    // BtclockDataSource are owned by the main task (Render holds a
    // reference into ScreenManager::currencies_ for the duration of a
    // frame; mutating that vector mid-render dangles the ref and crashes
    // — observed once on Rev B as a heap-corruption abort with sz read
    // as a DRAM ptr). Defer to main via the existing ControlCommand
    // queue: the main loop drains kRebuildScreens, re-reads NVS, and
    // applies the new currency list + rotation plan + WS subscriptions
    // in main-task context.
    ccfg.on_screens_changed = [ctx_ptr, main_task_for_hooks] {
      if (!ctx_ptr || !ctx_ptr->ctrl) return;
      ControlCommand cmd{ControlCommand::Kind::kRebuildScreens};
      ctx_ptr->ctrl->PostCommand(cmd);
      // Wake the main task so it drains the queue without waiting for
      // the 1 s heartbeat tick.
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
    // mdnsEnabled / hostnamePrefix PATCH hook — re-publish the mDNS
    // advert so the device responds under its new name (or stops
    // responding when the user toggles mdns off) without a reboot.
    // ReinitMdns owns the teardown + re-init dance; this lambda only
    // exists so the webserver component doesn't have to depend on
    // main/app/boot. Runs on the httpd worker, same as the other
    // on_*_changed hooks. bd btclock_v4-9ut.
    ccfg.on_mdns_changed = [] { ReinitMdns(); };
    // WebUI pause / resume buttons play the same NeoPixel sweep effect
    // the physical pause button already triggers, so users get the same
    // visual confirmation regardless of which surface they used.
    ccfg.on_rotation_paused_changed = [](bool now_paused) {
      PostLedEffect(now_paused ? LedEffect::kTimerPause
                               : LedEffect::kTimerResume);
    };
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
        // then schedule a reboot. The pre_flash_hook (fired before
        // esp_https_ota_begin) already latched ScreenManager into
        // OTA mode (panels frozen on "UPDATE!") and stopped every
        // data source — there's no symmetric on-failure path to
        // reverse those, and without a reboot the device stays
        // wedged showing the overlay forever. push-OTA failures
        // implicitly recover because the HTTP request returns to
        // the user and they retry; auto-update runs in a background
        // task with no caller waiting, so the only sane recovery is
        // to reboot. ~2 s of delay lets the ESP_LOGE failure line
        // flush over the USB-JTAG console first so the cause is
        // still in the buffer when the operator attaches a monitor.
        ShowOtaProgressLedCount(0);
        ESP_LOGW("ota-ux", "auto-update failed; rebooting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
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
  // Persist the cursor so a reboot resumes here instead of
  // defaulting to slot 0 (block-height). Skip on no-op transitions
  // to avoid burning NVS cycles on every status broadcast — the
  // function fires on every render, but the slot only changes on
  // navigation / auto-rotate / steal-focus events. Static state is
  // safe: PublishStatus only runs on the main task.
  static std::size_t last_persisted_slot = SIZE_MAX;
  const std::size_t cur = ctx.sm->current_slot();
  if (cur != last_persisted_slot) {
    Prefs rt(prefs::kRuntimeStateNs);
    rt.SetU32(prefs::kLastSlot, static_cast<uint32_t>(cur));
    last_persisted_slot = cur;
  }
  // Fan out to SSE subscribers so screen rotations / button presses
  // surface in the WebUI without waiting for the next poll.
  ctx.ctrl->BroadcastStatus();
}

}  // namespace btclock
