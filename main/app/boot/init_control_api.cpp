#include "app/boot/init_control_api.hpp"

#include <array>
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
#include "epd/panel.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if BTCLOCK_HAS_FRONTLIGHT
#include "io/frontlight_controller.hpp"
#endif
#include "io/led_controller.hpp"
#if BTCLOCK_HAS_BH1750
#include "io/light_sensor.hpp"
#endif
#include "io/mining_pool_selector.hpp"
#include "io/network_led_watchdog.hpp"
#include "nostr/nostr_data_source.hpp"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"
#include "nostr/zap_listener.hpp"
#include "nwc/client.hpp"
#include "nwc/queue.hpp"
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
  OtaManager::Config ocfg;
  // Read-through closure: each TriggerAutoUpdate / RunAutoUpdate
  // invocation pulls the live `gitReleaseUrl` from NVS, so a settings
  // PATCH takes effect on the very next attempt without a reboot.
  ocfg.release_url = []() {
    settings::NvsPrefs prefs(prefs::kSettingsNs);
    return btclock::settings::ReadString(prefs, prefs::kGitReleaseUrl);
  };
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
#if BTCLOCK_HAS_FRONTLIGHT
  if (ctx.frontlight) {
    ctx.fl_adapter = std::make_unique<FrontlightAdapter>(ctx.frontlight.get());
  }
#endif
  ctx.leds_adapter = std::make_unique<LedsAdapter>();
  ctx.dnd_adapter = std::make_unique<DndAdapter>();
#if BTCLOCK_HAS_BH1750
  if (ctx.light_sensor) {
    ctx.light_sensor_adapter =
        std::make_unique<LightSensorAdapter>(ctx.light_sensor.get());
  }
#endif
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
#if BTCLOCK_HAS_FRONTLIGHT
    ccfg.frontlight = ctx.fl_adapter.get();
#endif
    ccfg.leds = ctx.leds_adapter.get();
    ccfg.dnd = ctx.dnd_adapter.get();
    ccfg.timer = ctx.timer_adapter.get();
#if BTCLOCK_HAS_BH1750
    ccfg.light_sensor = ctx.light_sensor_adapter.get();
#endif
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
      epd::SetGlobalInverted(v);
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
#if BTCLOCK_HAS_FRONTLIGHT
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
        fl->SetOffOnDnd(
            btclock::settings::ReadBool(settings, prefs::kFlOffOnDnd));
      };
    }
#endif  // BTCLOCK_HAS_FRONTLIGHT
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
    // Runtime catalogues for GET /api/settings drop-downs — font ids and
    // ₿ cmap facts baked at font-regeneration time (see
    // catalog_available_font_catalog.gen.hpp).
    for (const auto& e : catalogs::kAvailableFontCatalog) {
      ccfg.available_fonts.push_back({std::string(e.id), e.has_btc_symbol});
    }
    // Currencies come from ctx (seeded in InitScreenManager from the
    // catalogue, then overwritten by FetchAvailableCurrencies on
    // dataSource=0/2). This way GET /api/settings reflects the upstream-
    // supported set instead of a compile-time array that can drift.
    ccfg.available_currencies = ctx.available_currencies;
    for (const auto& s : catalogs::kScreenKinds) {
      ccfg.screens_catalog.push_back(
          {s.api_id, std::string(s.display_label),
           slot_map::DefaultScreenVisible(s.api_id)});
    }
    // Per-relay Nostr liveness — read on every /api/status so the
    // WebUI's connection badge tracks reality. Walks both nostr_sources
    // (shared-WSS path: data source owns the RelayClient) and
    // zap_relays (dedicated-WSS path: zap listener owns its own when
    // dataSource != 2 or the URLs disagree). Each unique relay URL
    // appears exactly once: the shared path doesn't push to zap_relays
    // when a sibling NostrDataSource owns the same WSS, so dedup by
    // URL is implicit at construction time. Captures raw pointers to
    // AppCtx-owned vectors — both outlive the control server (AppCtx
    // declaration order keeps ctrl after every nostr field).
    AppCtx* ctx_ptr = &ctx;
    ccfg.nostr_relays_status = [ctx_ptr]() {
      std::vector<ControlServer::Config::NostrRelayStatus> out;
      out.reserve(ctx_ptr->nostr_sources.size() + ctx_ptr->zap_relays.size());
      for (auto* ds : ctx_ptr->nostr_sources) {
        if (!ds) continue;
        out.push_back({ds->relay_url(), ds->relay_connected()});
      }
      for (const auto& zr : ctx_ptr->zap_relays) {
        if (!zr) continue;
        out.push_back({zr->url(), zr->connected()});
      }
      return out;
    };
    // dataSource=1 plumbs price/blocks straight from the source's
    // per-WS connection probes. dataSource=0 leaves all three callbacks
    // unset → control_server falls back to the hub-presence heuristic
    // (which is correct for v2 since the single socket carries both).
    // bd btclock_v4-1xc.
    // NWC reachability — surfaces in /api/status `connectionStatus.nwc`
    // and feeds the WebUI badge. Reports true only after the wallet
    // service responded to the kind-13194 INFO event (state==kReady);
    // kBootstrapping / kIdle / kFatal all render as disconnected so
    // the user sees the same pill colour as a stalled relay. The
    // callback is wired unconditionally on every boot — the
    // /api/status emitter only attaches the field when the lambda is
    // present, so it's null on builds that don't expose NWC yet, and
    // we keep the indirection live so a runtime PATCH that flips
    // `nwcEnabled` reflects in the badge without a reboot.
    ccfg.nwc_connected = [ctx_ptr]() -> bool {
      if (!ctx_ptr || !ctx_ptr->nwc_enabled.load()) return false;
      if (!ctx_ptr->nwc_client) return false;
      return ctx_ptr->nwc_client->state() == nwc::State::kReady;
    };
    if (MempoolKrakenSource* mk = ctx.mempool_kraken) {
      ccfg.price_connected = [mk]() { return mk->IsKrakenConnected(); };
      ccfg.blocks_connected = [mk]() { return mk->IsMempoolConnected(); };
      ccfg.v2_connected = []() { return false; };
      // The LED-indicator probes are lifecycle-aware: ok when the
      // source is connected OR intentionally stopped. /api/status's
      // probes above stay wire-truth so the WebUI badge still flips
      // on a real outage, but the LED watchdog needs to stay quiet
      // through every Stop() (OTA quiesce, /api/stop_datasources,
      // factory reset, dataSource toggle). bd btclock_v4-28n.
      if (auto* nlw = ctx.network_led_watchdog.get()) {
        nlw->SetPriceConnected([mk]() {
          return !mk->IsKrakenActive() || mk->IsKrakenConnected();
        });
        nlw->SetBlocksConnected([mk]() {
          return !mk->IsMempoolActive() || mk->IsMempoolConnected();
        });
      }
    } else if (BtclockDataSource* bws = ctx.btclock_ws) {
      // dataSource=0: the single v2 WSS carries price + blocks + fees,
      // so every "is X reachable" callback resolves to the same state.
      // Pre-fix these were all unwired and ControlServer's emit fell
      // back to "hub != nullptr" — always true, never reflected an
      // actual outage. Now wired to the WS-layer CONNECTED/DISCONNECTED
      // mirror in BtclockDataSource so /api/status's badge tracks the
      // real socket state. Note: this still doesn't catch a silent
      // subscription drop where the socket is alive but blockheight
      // stops flowing — that case is covered by the staleness watchdog
      // (see bd btclock_v4-lfd). bd btclock_v4-1xc.
      ccfg.price_connected = [bws]() { return bws->IsConnected(); };
      ccfg.blocks_connected = [bws]() { return bws->IsConnected(); };
      ccfg.v2_connected = [bws]() { return bws->IsConnected(); };
      // Lifecycle-aware LED probes — see the equivalent MempoolKraken
      // branch above. Stop() clears client_ so IsActive() goes false
      // and the watchdog falls silent; once Start() runs again and
      // the WS reconnects, IsConnected() flips true and the steady
      // state is "ok" without any in-between red breath.
      if (auto* nlw = ctx.network_led_watchdog.get()) {
        nlw->SetPriceConnected(
            [bws]() { return !bws->IsActive() || bws->IsConnected(); });
        nlw->SetBlocksConnected(
            [bws]() { return !bws->IsActive() || bws->IsConnected(); });
      }
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
      // Persist run/pause across reboots (Forgejo #3) — fires on the
      // httpd worker from both /api/action/pause and
      // /api/action/timer_restart, but only on an actual transition.
      PersistTimerActive(now_paused);
      PostLedEffect(now_paused ? LedEffect::kTimerPause
                               : LedEffect::kTimerResume);
    };
    // POST /api/action/simulate_zap — drives the same code path the
    // real zap listener uses so HA's "Simulate Zap" button actually
    // exercises the LED strip + frontlight too, not just the screen
    // overlay. The screen overlay fires unconditionally (it's the
    // primary "did the action work?" signal for QA / automation
    // wiring), but the physical effects honour their per-effect
    // user gates — ledFlashOnZap and flFlashOnZap — so flipping
    // either toggle is observable from a simulated event.
    //
    // Effects (LED + frontlight) are NOT fired here. They fire from
    // event_loop.cpp's zap-notify dispatcher AFTER sm.Render() paints
    // the overlay, exactly like the BindOnZap callback does for real
    // zaps. Firing them here too would double-flash (once now, once
    // post-render) and break the LED↔frontlight↔EPD coordination the
    // dispatcher establishes. bd btclock_v4-8a4.
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
      if (api_id != slot_map::kApiIdMiningPoolEarnings &&
          api_id != slot_map::kApiIdMiningPoolEstimatedEarnings) {
        return false;
      }
      Prefs p(prefs::kSettingsNs);
      const std::string name =
          btclock::settings::ReadString(p, prefs::kMiningPoolName);
      if (api_id == slot_map::kApiIdMiningPoolEarnings) {
        return !mining_pools::PoolSupportsDailyEarnings(name);
      }
      // kApiIdMiningPoolEstimatedEarnings — gated separately so a
      // pool that has settled-earnings (Ocean) can still hide the
      // estimate slot (no PPLNS window to project from).
      return !mining_pools::PoolSupportsEstimatedEarnings(name);
    };
    // Body-first POST /api/show/screen {"s":<api_id>} and the
    // `currentScreen` field in
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
    // Wire the v2 WS staleness watchdog's restart hook now that
    // ctx.ctrl exists. The watchdog detects "WS alive but blockheight
    // stream silently dropped" inside BtclockDataSource and used to
    // call esp_websocket_client_close() directly. That close-only path
    // failed to recover on multi-panel — observed live on V8 + Rev B
    // 2026-05-22 with 50+ hours of stale blocks while price ticks kept
    // flowing. Posting kRestartDataSources here mirrors the mini-
    // variant fix in 93135b8 and the manual /api/restart_datasources
    // recovery that proven to work. Main task drains the command and
    // does hub->StopAll() + hub->StartAll() — full BtclockDataSource
    // teardown + re-init, which clears whatever subscription state the
    // relay held. bd btclock_v4-lfd.
    if (ctx.ctrl && ctx.btclock_ws) {
      ControlServer* ctrl_for_restart = ctx.ctrl.get();
      ctx.btclock_ws->SetRestartRequester([ctrl_for_restart]() {
        ControlCommand cmd{ControlCommand::Kind::kRestartDataSources};
        ctrl_for_restart->PostCommand(cmd);
      });
    }
  }

  // OTA manager — stores release URL + per-variant asset name so the
  // /api/firmware/auto_update handler can kick off a pull update.
  GetOtaManager().Init(MakeOtaConfig());

  // Pre-flash hook: fires once on a worker task (auto-update spawns
  // its own ota_auto task; push-OTA fires on the httpd worker) before
  // esp_ota_begin erases the partition. We stop every data source
  // (WS client, nostr relay + zap listener, mining-pool + bitaxe
  // pollers) to free the internal heap their TLS / recv buffers were
  // holding, then hand off the "UPDATE!" overlay paint to the main
  // task and latch ScreenManager into OTA mode so the main loop won't
  // stomp it on a subsequent data push. The main render loop stops
  // painting once SetOtaOverlay(true) is observed — subsequent panel
  // writes come only from this handler's completion-blink branch (LED
  // only; the EPD stays on the "UPDATE!" screen until esp_restart).
  //
  // The overlay render is dispatched onto the main task (not painted
  // inline) because font.cpp's glyph_buf is owned by whichever task
  // first claims it — that's always the main task, and a second
  // claimant aborts. We set ota_overlay_render_pending, kick the main
  // task, and block on the rendered semaphore so the OTA flow only
  // begins erasing the partition after the EPD shows UPDATE.
  GetOtaManager().SetPreFlashHook([ctx_ptr]() {
    ESP_LOGW("ota-ux", "pre-flash hook: painting UPDATE + quiescing data");
    // Paint the "UPDATE!" overlay FIRST so the user sees it within a
    // single EPD refresh, regardless of how long the data quiesce
    // below takes. Safe to reorder: SetOtaOverlay(true) makes
    // ShouldRender() return false, so even if a data source pushes
    // one last frame before its Stop() returns, the main loop won't
    // stomp the overlay. The 5 s deadline is generous — the actual
    // render is a static-text paint (<300 ms on a 2.13"), but a
    // partial-refresh that hit just before the hook fired can hold
    // the EPD bus for ~1.7 s; missing the deadline is still
    // recoverable (EPD just won't show "UPDATE!" but the LED bar
    // still indicates progress).
    if (ctx_ptr->sm) {
      ctx_ptr->sm->SetOtaOverlay(true);
      ctx_ptr->ota_overlay_render_pending.store(true);
      if (ctx_ptr->main_task != nullptr) {
        xTaskNotifyGive(ctx_ptr->main_task);
      }
      if (ctx_ptr->ota_overlay_rendered_sem != nullptr) {
        if (xSemaphoreTake(ctx_ptr->ota_overlay_rendered_sem,
                           pdMS_TO_TICKS(5000)) != pdTRUE) {
          ESP_LOGW("ota-ux",
                   "main task did not render OTA overlay within 5s; "
                   "proceeding with flash anyway");
        }
      }
    }
    // Order is non-obvious — see ota_quiesce.hpp. Host-tested in
    // test_ota_quiesce.cpp. Multi-relay: stop EVERY listener before any
    // relay (they may borrow a sibling NostrDataSource's
    // SubscriptionManager — the data source dies inside hub->StopAll()
    // last), then stop every dedicated relay, then the hub. Walks all
    // listeners with a null relay so QuiesceOtaPreFlash's three-step
    // ordering holds for the dedicated case below.
    for (auto& listener : ctx_ptr->zap_listeners) {
      if (listener) listener->Stop();
    }
    for (auto& relay : ctx_ptr->zap_relays) {
      if (relay) relay->Stop();
    }
    if (ctx_ptr->hub) ctx_ptr->hub->StopAll();
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
  // Mirror the exact rendered 1-bit framebuffers into the preview WS
  // side-channel. This is "latest wins": if rendering outruns network
  // IO the control-server worker keeps only the newest snapshot.
  std::array<ControlServer::FramebufferPanelView, board::kNumPanels> views;
  size_t panel_count = 0;
  auto& fb_storage = AppCtx::fb_storage();
  for (size_t i = 0; i < static_cast<size_t>(board::kNumPanels); ++i) {
    if (!ctx.panels[i]) continue;
    ControlServer::FramebufferPanelView v = {};
    v.panel_index = static_cast<uint8_t>(i);
    v.width = static_cast<uint16_t>(ctx.panels[i]->Width());
    v.height = static_cast<uint16_t>(ctx.panels[i]->Height());
    v.stride = static_cast<uint16_t>(ctx.panels[i]->Stride());
    v.rotation_deg = 180;
    v.data = fb_storage[i];
    v.data_bytes = static_cast<size_t>(ctx.panels[i]->FrameBytes());
    views[panel_count++] = v;
  }
  if (panel_count > 0) {
    const uint64_t ts_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    ctx.ctrl->PublishFramebufferSnapshot(views.data(), panel_count, ts_ms);
  }
  // Fan out to SSE subscribers so screen rotations / button presses
  // surface in the WebUI without waiting for the next poll.
  ctx.ctrl->BroadcastStatus();
}

}  // namespace btclock
