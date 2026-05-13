#include "app/event_loop.hpp"

#include <cstdint>
#include <string>

#include "app/app_ctx.hpp"
#include "app/block_event_policy.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/control_command_drain.hpp"
#include "app/screen_manager.hpp"
#include "board/board.hpp"
#include "buttons.hpp"
#include "data_core/hub.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "io/light_sensor.hpp"
#include "io/network_led_watchdog.hpp"
#include "io/wifi_guard.hpp"
#include "lwip/sockets.h"
#include "mcp23017.hpp"
#include "nwc/client.hpp"
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
  auto& nwc_notify_pending = ctx.nwc_notify_pending;
  auto& nwc_refresh_pending = ctx.nwc_refresh_pending;
  auto& nwc_notify_auto_restore = ctx.nwc_notify_auto_restore;
  auto& nwc_flash_on_payment_enabled = ctx.nwc_flash_on_payment_enabled;
  auto& ota_overlay_render_pending = ctx.ota_overlay_render_pending;
  SemaphoreHandle_t ota_overlay_rendered_sem = ctx.ota_overlay_rendered_sem;
  auto& flash_frontlight_on_zap_enabled = ctx.flash_frontlight_on_zap_enabled;
  auto& flash_on_zap_enabled = ctx.flash_on_zap_enabled;
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
    // Per-iteration latch: the new-block decision sets this true, the
    // render path that paints the new height kicks BOTH the LED block-
    // flash effect AND the frontlight staggered pulse afterwards (in
    // that order, but as close together as the queue dispatch allows).
    // Coordinating the two effects post-render means the user perceives
    // a single "new block!" notification — digits appear, panels pulse,
    // LED ring strobes — instead of an LED-leads-EPD stagger that was
    // most jarring on partial refreshes (1-digit increment is just
    // ~120 ms of EPD work, but the LED firing first stretched the
    // perceived gap). Reset at the top of every iteration so a
    // `continue` mid-iteration can't carry the flag forward and fire
    // spuriously next time around.
    bool pending_block_notify = false;

    // Drain control-API commands first. These ride in on the httpd
    // worker task via the ControlServer's queue, not the button queue,
    // so they need their own drain step. The dispatch logic lives in
    // app/control_command_drain.cpp; the helper coalesces the post-
    // drain Render and publish_status. Returning true means at least
    // one command was handled and the loop should `continue` to honour
    // the one-action-per-pass invariant.
    if (DrainControlCommands(ctx)) {
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
    // whole point is that there's no STA connection to watch. Same
    // skip applies to the LED-indicator watchdog: AP mode already runs
    // the kSetProvisioning breathe and we don't want to clobber it
    // with a "wifi down" red breath.
    if (!wifi.is_ap_mode()) {
      outage_watchdog.Tick(wifi, static_cast<uint32_t>(now_ms));
      if (ctx.network_led_watchdog) {
        ctx.network_led_watchdog->Tick(static_cast<uint32_t>(now_ms));
      }
    }

    // DND edge detection for the frontlight: Post()'s suppressor gate
    // only catches new commands, so an already-on backlight stays lit
    // when DND turns on (manual toggle or time-based crossing) until
    // something else nudges the queue. Driving this from the 1 Hz
    // wake-up gives a worst-case 1 s lag on minute-aligned time-based
    // DND boundaries, and near-immediate response on manual toggles.
    if (frontlight) frontlight->OnDndStateMaybeChanged();

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
      // Socket-FD census + heap, emitted at WARN so it survives the
      // CONFIG_LOG_MAXIMUM_LEVEL_WARN strip. Hunting an LWIP socket
      // leak: at 8 h uptime Rev B exhausted CONFIG_LWIP_MAX_SOCKETS
      // (accept(23)=ENFILE, esp-tls socket() fails). This line is the
      // diagnostic hook to attribute the leak to a specific subsystem.
      {
        int used = 0;
        for (int fd = LWIP_SOCKET_OFFSET;
             fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
          int type = 0;
          socklen_t len = sizeof(type);
          if (lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
            ++used;
          }
        }
        ESP_LOGW(
            kTag, "diag t=%llds sockets=%d/%d heap=%u largest=%u psram=%u",
            static_cast<long long>(now_ms / 1000), used,
            CONFIG_LWIP_MAX_SOCKETS,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      }
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

    // OTA "UPDATE!" overlay: the pre-flash hook fires on the ota_auto
    // worker (auto-update) or the httpd worker (push-OTA) and raises
    // this flag instead of rendering directly. Doing the render here
    // keeps the font.cpp single-thread invariant intact (glyph_buf
    // ownership stays with the main task) and avoids the silent
    // scratch-buffer corruption the cross-task path produced before
    // the abort guard caught it. We post the semaphore at the end so
    // the hook returns only AFTER the overlay is on the EPDs; OTA can
    // then safely begin its destructive partition erase.
    bool pending_ota_overlay = true;
    if (ota_overlay_render_pending.compare_exchange_strong(pending_ota_overlay,
                                                           false)) {
      RenderOtaUpdateScreen(panels, fb_storage, fonts);
      if (ota_overlay_rendered_sem != nullptr) {
        xSemaphoreGive(ota_overlay_rendered_sem);
      }
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
      // LED + frontlight zap notification fires AFTER the overlay paints
      // — same coordination rule as the new-block path above. Render()
      // is synchronous (PaintDataScreen waits per panel), so by the time
      // we get here the user sees the zap screen, then the LED ring +
      // staggered frontlight pulse fire together. The relay-worker
      // callback that raised zap_notify_pending intentionally skips both
      // effects — see init_zap_listener.cpp BindOnZap. Per-effect gates
      // (ledFlashOnZap / flFlashOnZap) are evaluated here so a live
      // PATCH lands without a reboot.
      if (flash_on_zap_enabled.load()) {
        PostLedEffect(LedEffect::kZap);
      }
      if (frontlight && flash_frontlight_on_zap_enabled.load()) {
        frontlight->ZapFlash();
      }
      continue;
    }

    // NWC periodic balance refresh. The esp_timer task only flips the
    // flag + xTaskNotifyGive — the heavy NIP-44 encrypt + schnorr sign
    // + JSON build that `RequestGetBalance()` performs runs HERE on
    // the main task instead, where the stack is sized for the full
    // render path. Direct dispatch from esp_timer (default ~3.5 KiB
    // stack) used to abort the device every nwcRefreshSecs.
    bool pending_refresh = true;
    if (nwc_refresh_pending.compare_exchange_strong(pending_refresh, false)) {
      if (ctx.nwc_client && ctx.nwc_client->state() == nwc::State::kReady) {
        ctx.nwc_client->RequestGetBalance();
      }
    }

    // NWC payment notification — same pattern as the zap branch above.
    // Main loop owns the ScreenManager mutation; the relay callback
    // already wrote the snapshot patch via DataHub, so by the time
    // SetNwcPaymentNotify() lands the renderer's nwc_last_payment is
    // current. The master `nwcShowPaymentNotify` toggle short-circuits
    // BOTH the overlay AND the flash so a privacy-sensitive deployment
    // can hide payment events from bystanders entirely. The per-effect
    // flash gate `nwcFlashOnPay` only applies when notifications are
    // shown at all. Both prefs are re-read on dispatch so a live PATCH
    // lands without reboot.
    bool pending_nwc = true;
    if (nwc_notify_pending.compare_exchange_strong(pending_nwc, false) && hub) {
      Prefs nwc_prefs(prefs::kSettingsNs);
      const bool show_notify =
          btclock::settings::ReadBool(nwc_prefs, prefs::kNwcShowNotify);
      if (!show_notify) {
        // Balance / snapshot mirror already updated in the relay
        // callback — nothing more to do here. Skip overlay + flash so
        // the device stays on whatever screen the rotation was on.
        continue;
      }
      const int64_t timer_s =
          static_cast<int64_t>(nwc_prefs.GetU32(prefs::kTimerSeconds, 0));
      const int64_t timeout_ms = ZapOverlayPolicy::ComputeTimeoutMs(timer_s);
      sm.SetNwcPaymentNotify(now_ms, nwc_notify_auto_restore.load(),
                             timeout_ms);
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      if (nwc_flash_on_payment_enabled.load()) {
        PostLedEffect(LedEffect::kZap);
        if (frontlight) frontlight->ZapFlash();
      }
      continue;
    }

    // timerSeconds drives auto-rotate cadence. Read per iteration so a
    // live PATCH lands without reboot (same pattern as kStealFocus
    // below). Zero disables rotation entirely — ShouldAdvance with
    // period_ms=0 would otherwise fire on every tick. Default lives in
    // pref_keys.hpp (`kDefaultTimerSeconds`); BuildGetResponse uses the
    // same constant so /api/settings can't drift from runtime behaviour.
    Prefs rotate_prefs(prefs::kSettingsNs);
    const uint32_t timer_s =
        rotate_prefs.GetU32(prefs::kTimerSeconds, prefs::kDefaultTimerSeconds);
    const int64_t auto_rotate_ms = static_cast<int64_t>(timer_s) * 1000;
    if (auto_rotate_ms > 0 && sm.MaybeAutoRotate(now_ms, auto_rotate_ms) &&
        hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    if (!hub) continue;
    const auto snap = hub->GetSnapshot();

    uint32_t prev_block_height = 0;
    if (sm.ConsumeNewBlock(snap, &prev_block_height)) {
      // Persist the new height so the next reboot's
      // SeedLastSeenHeight reads back a non-zero value and the very
      // first WS frame after boot is treated as a real new-block
      // event (catch-up if delta>100, fresh if delta<=100). NVS
      // writes are cheap on the heartbeat cadence of new blocks
      // (~1 / 10 min), so no batching is needed. Mirrors v3 commit
      // 989e645. Static so the open is hot; same task as the rest
      // of the event loop.
      const uint32_t new_block_height =
          snap.block_height ? *snap.block_height : 0;
      if (new_block_height != 0) {
        static Prefs rt(prefs::kRuntimeStateNs);
        rt.SetU32(prefs::kLastBlockHeight, new_block_height);
      }
      // Catch-up gate: when the device boots after being offline (or
      // after a long WS reconnect window), the first snapshot can jump
      // hundreds of blocks ahead of last_seen_height_. Treat that as
      // "catching up to chain tip" rather than a realtime new-block
      // event — the user shouldn't get a LED strobe + frontlight pulse
      // + steal-focus yank for what is effectively a startup snapshot.
      // The screen still re-renders via the normal ShouldRender path
      // below, so the new height does appear.
      const bool catch_up =
          BlockEventPolicy::IsCatchUpJump(prev_block_height, new_block_height);
      if (!catch_up) {
        // Both the LED block-flash effect and the frontlight staggered
        // pulse are deferred until AFTER the render that paints the new
        // height — see the latch comment above the loop for the
        // coordination rationale. PaintDataScreen is synchronous
        // (WaitForRefresh per panel), so completing sm.Render() is
        // sufficient; v3 firmware's `partial_refresh_time × NUM_SCREENS
        // + MCP23017 fudge` delay collapses here because the panel
        // driver already blocks on the BUSY line.
        pending_block_notify = true;
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
            if (pending_block_notify) {
              PostLedEffect(LedEffect::kBlockFlash);
              if (frontlight) frontlight->Flash();
            }
            continue;
          }
        } else if (BlockEventPolicy::ShouldRestartTimerOnBlockUpdate(
                       steal_focus, sm.current_kind())) {
          // Already on kBlockHeight when the new block landed. The
          // steal path is a no-op (we're on the right screen), but the
          // rotation deadline keeps ticking — without resetting it the
          // user can see the fresh height for as little as a few hundred
          // milliseconds before the next slot rotates in. Restart the
          // timer so the new digits stay visible for the full
          // `timerSeconds` window. The render itself happens in the
          // ShouldRender branch below; we just adjust the deadline here.
          sm.RestartTimer(now_ms);
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
    // Non-steal-focus path of a new-block event lands here: the render
    // above (if it ran) just painted the new height on whatever screen
    // is current (typically kBlockHeight if it was already showing,
    // where only one or two digit panels actually need a partial
    // refresh — fast). Either way, the EPD refresh has completed by
    // the time Render() returns, so kicking the LED + frontlight flash
    // now matches the user-visible "new digits, then the notification
    // fires" sequence. If there was no render (current screen
    // unaffected by the new height), we still notify — the user opted
    // into "alert me on every block".
    if (pending_block_notify) {
      PostLedEffect(LedEffect::kBlockFlash);
      if (frontlight) frontlight->Flash();
      pending_block_notify = false;
    }
  }
}

}  // namespace btclock
