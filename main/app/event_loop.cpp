#include "app/event_loop.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "app/app_ctx.hpp"
#include "app/block_event_policy.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/screen_manager.hpp"
#include "board/board.hpp"
#include "buttons.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "io/light_sensor.hpp"
#include "io/wifi_guard.hpp"
#include "mcp23017.hpp"
#include "prefs.hpp"
#include "screens/screens.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

[[noreturn]] void RunEventLoop(AppCtx& ctx) {
  // Locals mirroring the old app_main aliases so this TU reads like
  // the original event-loop block.
  ScreenManager& sm = *ctx.sm;
  auto& hub = ctx.hub;
  auto& ctrl = ctx.ctrl;
  auto& panels = ctx.panels;
  auto& fb_storage = AppCtx::fb_storage();
  auto& fonts = ctx.fonts;
  auto& frontlight = ctx.frontlight;
  auto& light_sensor = ctx.light_sensor;
  Wifi& wifi = *ctx.wifi;
  Mcp23017& mcp = *ctx.mcp;
  OutageWatchdog& outage_watchdog = *ctx.outage_watchdog;
  QueueHandle_t button_q = ctx.button_q;
  auto& zap_notify_pending = ctx.zap_notify_pending;
  auto& zap_screen_auto_restore = ctx.zap_screen_auto_restore;
  const std::string& ssid = ctx.sta_ssid;
  auto publish_status = [&ctx]() { PublishStatus(ctx); };

  // Three wake sources: data-push notify, button event, 1 s heartbeat
  // tick. We drain the button queue first so a queued click is honoured
  // before we sleep on the task-notify.
  int64_t last_heartbeat_ms = 0;
  int64_t last_debug_render_ms = 0;

  auto render_debug = [&](int64_t now_ms, bool force_full) {
    DebugScreenInfo info;
    info.ip = wifi.ip();
    info.ssid = ssid;
    info.free_heap =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    info.free_psram =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    info.hw_name = board::kHardwareName;
    {
      const esp_app_desc_t* app = esp_app_get_description();
      info.fw_version = (app && app->version[0]) ? app->version : "unknown";
    }
    info.built = __DATE__;
    info.uptime_s = static_cast<uint32_t>(now_ms / 1000);
    sm.RenderDebug(panels, fb_storage, fonts, info, now_ms, force_full);
    last_debug_render_ms = now_ms;
  };

  while (true) {
    // Drain control-API commands first. These ride in on the httpd
    // worker task via the ControlServer's queue, not the button queue,
    // so they need their own drain step. A single iteration handles
    // one command to keep the event loop's "one action per pass"
    // contract intact (rotations, rendering, etc. in the same pass).
    ControlCommand ccmd{};
    if (ctrl && ctrl->TryPopCommand(&ccmd)) {
      using Kind = ControlCommand::Kind;
      bool re_render = false;
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
      }
      if (re_render && hub)
        sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    ButtonInput ev{};
    if (xQueueReceive(button_q, &ev, 0) == pdTRUE) {
      // Only the falling-edge kClick drives actions today; kLongPress
      // events are intentionally dropped until someone finds them a
      // semantic. Mirrors the brief: "ignore long-press for now".
      if (ev.event != ButtonEvent::kClick) continue;
      bool re_render = false;
      bool show_debug = false;
      switch (ev.id) {
        case ButtonId::k0:
          // Pause / resume auto-rotate. The current slot stays up.
          sm.TogglePaused();
          ESP_LOGI(kTag, "button: pause=%d", sm.IsPaused() ? 1 : 0);
          PostLedEffect(sm.IsPaused() ? LedEffect::kTimerPause
                                      : LedEffect::kTimerResume);
          break;
        case ButtonId::k1:
          // Next screen — same as auto-rotate step, Restart() resets
          // the rotation deadline so we don't immediately advance again.
          if (!sm.IsDebug()) {
            sm.NextScreen(MsNow());
            re_render = true;
          }
          break;
        case ButtonId::k2:
          if (!sm.IsDebug()) {
            sm.PrevScreen(MsNow());
            re_render = true;
          }
          break;
        case ButtonId::k3: {
          // Toggle debug overlay. Entry renders via RenderDebug (full-
          // refresh, no data snapshot); exit marks dirty so the normal
          // render path repaints the underlying data slot.
          const bool now_debug = sm.ToggleDebug(MsNow());
          show_debug = now_debug;
          re_render = !now_debug;
          break;
        }
      }
      if (show_debug) {
        render_debug(MsNow(), /*force_full=*/true);
      } else if (re_render && hub) {
        sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      }
      publish_status();
      continue;
    }

    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const int64_t now_ms = MsNow();

    // Soft watchdog pump — skipped in AP/provisioning mode, where the
    // whole point is that there's no STA connection to watch.
    if (!wifi.is_ap_mode()) {
      outage_watchdog.Tick(wifi, static_cast<uint32_t>(now_ms));
    }

    if (now_ms - last_heartbeat_ms >= 10'000) {
      uint16_t port = 0;
      mcp.ReadPort(&port);
      // Prefer the cached reading from the poll task so we don't race
      // on the I2C bus with it. Falls back to -1.0 when no sensor.
      const float lux = light_sensor ? light_sensor->GetLux() : -1.0f;
      ESP_LOGI(
          kTag, "t=%llds buttons=0x%X lux=%.1f heap=%u psram=%u",
          static_cast<long long>(now_ms / 1000),
          static_cast<unsigned>(port & 0xF), lux,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      // Auto-off: feed each fresh lux reading to the frontlight
      // controller. Below threshold -> on, above -> off. No-op if the
      // board has no frontlight or no ambient sensor.
      if (frontlight) frontlight->OnAmbientLux(lux);
      last_heartbeat_ms = now_ms;
    }

    // Debug overlay auto-refresh: while the debug screen is up, repaint
    // every 10 s so uptime / free-heap / free-psram stay live without
    // requiring a button press. Skips silently when the overlay is not
    // active so the rest of the loop is unaffected.
    if (sm.IsDebug() && now_ms - last_debug_render_ms >= 10'000) {
      render_debug(now_ms, /*force_full=*/false);
      publish_status();
      continue;
    }

    // Zap notification: push the overlay onto ScreenManager from the
    // main task. The atomic flag was raised by the zap listener's worker-
    // thread callback, so doing the mutation here keeps ScreenManager
    // single-threaded. Expected bool for exchange — clear the flag
    // before dispatching.
    bool pending_zap = true;
    if (zap_notify_pending.compare_exchange_strong(pending_zap, false) && hub) {
      // timerSeconds (seconds) drives the overlay's visible-time
      // window when scrnRestoreZap=true. Read at dispatch time so a
      // live PATCH lands without a reboot; ZapOverlayPolicy clamps 0
      // to the documented fallback so the zap doesn't vanish before
      // the viewer reads it.
      Prefs zap_prefs(prefs::kSettingsNs);
      const int64_t timer_s =
          static_cast<int64_t>(zap_prefs.GetU32(prefs::kTimerSeconds, 0));
      const int64_t timeout_ms = ZapOverlayPolicy::ComputeTimeoutMs(timer_s);
      sm.SetZapNotify(now_ms, zap_screen_auto_restore.load(), timeout_ms);
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    // timerSeconds drives auto-rotate cadence. Read per iteration so a
    // live PATCH lands without reboot (same pattern as kStealFocus
    // below). Zero disables rotation entirely — ShouldAdvance with
    // period_ms=0 would otherwise fire on every tick.
    Prefs rotate_prefs(prefs::kSettingsNs);
    const uint32_t timer_s = rotate_prefs.GetU32(prefs::kTimerSeconds, 30);
    const int64_t auto_rotate_ms = static_cast<int64_t>(timer_s) * 1000;
    if (auto_rotate_ms > 0 && sm.MaybeAutoRotate(now_ms, auto_rotate_ms) &&
        hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    if (!hub) continue;
    const auto snap = hub->GetSnapshot();

    if (sm.ConsumeNewBlock(snap)) {
      PostLedEvent(LedEvent::kBlockFlash);
      if (frontlight) frontlight->Flash();
      // stealFocus: when enabled, a new block jumps the display to the
      // block-height screen so the viewer sees the fresh height without
      // waiting for rotation. Pref read per-event so a live PATCH lands
      // without reboot. Overlay-aware — debug / custom / zap overlays
      // block the steal (see BlockEventPolicy::ShouldSteal).
      Prefs block_prefs(prefs::kSettingsNs);
      const bool steal_focus =
          btclock::settings::ReadBool(block_prefs, prefs::kStealFocus);
      if (BlockEventPolicy::ShouldSteal(steal_focus, sm.current_kind())) {
        if (sm.SetKind(ScreenType::kBlockHeight, now_ms)) {
          sm.Render(panels, fb_storage, fonts, snap);
          publish_status();
          continue;
        }
      }
    }

    if (got != 0 && sm.ShouldRender(snap)) {
      sm.Render(panels, fb_storage, fonts, snap);
      // Data-push renders (incl. the per-minute clock tick) update
      // ScreenManager::last_panel_texts_ but previously didn't flow
      // into the ControlServer's cached LiveStatus. Without this
      // /api/status.data[] and the SSE "status" event would stay stuck
      // on whatever was current at the last nav/rotation — most
      // visibly on the Time screen, which re-renders every minute
      // without any accompanying nav event.
      publish_status();
    }
  }
}

}  // namespace btclock
