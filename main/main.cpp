// BTClock v4 — orchestrator.
//
// app_main is wire-up only: a sequence of Init* calls that populate
// AppCtx, then hand off to the long-running event loop. Each subsystem
// owns its own TU:
//
//   app/boot/init_boot_leds  — boot banner + LED task + DND suppressor
//   app/boot/init_hardware   — I2C + MCP + PCA + light sensor
//   app/boot/init_panels     — EPD panels + splash
//   app/boot/init_storage    — NVS + LittleFS + TZ
//   app/boot/init_network    — Wi-Fi STA vs SoftAP + provisioning portal
//   app/boot/init_screen_manager — ScreenManager + buttons
//   app/boot/init_boot_path  — dispatch: provisioning overlay vs data wiring
//   app/boot/init_control_api    — HTTP control API + SSE + OTA
//   app/boot/init_mdns       — advertise device over mDNS (STA only)
//   app/event_loop           — long-running render + event pump
//
// Board variant comes from -DBTCLOCK_BOARD=REV_A|REV_B|V8 at build
// time. Panel geometry is orthogonal and unconstrained:
// -DBTCLOCK_PANEL=2_13|2_9|7_5 picks the EPD driver independently of
// the board pin map. 2.13" is the default for every board.

#include "app/app_ctx.hpp"
#include "app/boot/init_boot_leds.hpp"
#include "app/boot/init_boot_path.hpp"
#include "app/boot/init_cjson_psram.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/boot/init_hardware.hpp"
#include "app/boot/init_mbedtls_psram.hpp"
#include "app/boot/init_mdns.hpp"
#include "app/boot/init_network.hpp"
#include "app/boot/init_panels.hpp"
#include "app/boot/init_screen_manager.hpp"
#include "app/boot/init_storage.hpp"
#include "app/boot/init_wifi_reset_button.hpp"
#include "app/event_loop.hpp"
#include "boot_spinner.hpp"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sources/sources.hpp"
#include "wifi.hpp"

extern "C" void app_main() {
  // Bump main-task priority above the network/LWIP stack (LWIP is at
  // 18, WiFi at 23, esp_timer at 22). The EPD render path runs
  // synchronously in this task, and the SSD1680 driver's per-frame
  // command sequence + BUSY polling assumes prompt scheduling. At the
  // default priority 1 the render gets preempted by every network
  // worker (SSE broadcast, mining-pool poll, WSS keep-alive, OTA
  // chunk handler), which serialises panel updates against arbitrary
  // network bursts and produces partial-refresh ghosting under load.
  // 20 keeps WiFi (23) and esp_timer (22) ahead of us so RF + tick
  // servicing aren't starved.
  vTaskPrioritySet(nullptr, 20);

  // Coredump partition is registered before any subsystem can panic so
  // a crash during boot is still captured. esp_core_dump_init() is
  // idempotent and cheap; the log line surfaces a stale dump from the
  // previous session so a field user can be told to GET /api/coredump
  // before the next crash overwrites it.
  esp_core_dump_init();
  if (esp_core_dump_image_check() == ESP_OK) {
    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) == ESP_OK) {
      // Log the CURRENT app's ELF SHA next to the coredump notice so
      // operators can spot SHA-mismatch decode problems before they
      // burn an hour reverse-engineering an unreliable backtrace. The
      // coredump partition embeds its own app_elf_sha256 (the crashing
      // build's SHA); espcoredump.py compares that against the ELF
      // passed on the command line. If the device was re-flashed
      // between crash + decode, those SHAs don't match and the decoded
      // frames past panic-handler are symbol-resolution illusions
      // against the wrong binary. See btclock_v4-ajf.
      const esp_app_desc_t* app = esp_app_get_description();
      char sha_prefix[9] = {};
      if (app != nullptr) {
        for (int i = 0; i < 4; ++i) {
          static const char kHex[] = "0123456789abcdef";
          sha_prefix[i * 2 + 0] = kHex[(app->app_elf_sha256[i] >> 4) & 0xF];
          sha_prefix[i * 2 + 1] = kHex[app->app_elf_sha256[i] & 0xF];
        }
      }
      ESP_LOGE("boot",
               "coredump from previous run present (%u bytes); current app "
               "ELF SHA prefix=%s — pull /api/coredump BEFORE next re-flash, "
               "and use this build's build-<variant>/btclock_v4.elf to decode "
               "(SHA mismatch = unreliable decode past frame #3)",
               static_cast<unsigned>(size), sha_prefix);
    }
  }

  // Route every cJSON node alloc through PSRAM. Must run before any
  // cJSON_Parse fires — that includes settings load (InitStorage →
  // ApplyPatch on persisted blob), so register the hook before *any*
  // Init* call. See init_cjson_psram.cpp for the rationale.
  btclock::InitCjsonPsram();

  // Route every mbedTLS calloc/free through PSRAM. Must run before
  // any TLS handshake — that includes WiFi WPA2 (in InitNetwork) and
  // every later HTTPS/WSS connection. Mitigates the
  // esp_crt_ca_cb_callback leak on the cert-bundle verify path; see
  // init_mbedtls_psram.cpp for the heap_trace evidence + upstream
  // bug-filing pointer.
  btclock::InitMbedtlsPsram();

  btclock::InitBootLeds();

  btclock::AppCtx ctx;
  btclock::InitHardware(ctx);
  btclock::InitPanelsAndSplash(ctx);
  btclock::InitStorage(ctx);
  // Boot-time button-1 hold path: clears STA creds + reboots into
  // SoftAP if held continuously for 3 s. Runs AFTER InitStorage so
  // NVS is open, BEFORE InitNetwork so the creds we're about to wipe
  // haven't been used to associate yet. Cheap fall-through when the
  // button isn't held.
  btclock::MaybeWifiResetAtBoot(ctx);
  btclock::InitNetwork(ctx);
  // Splash stayed painted through hardware bring-up + WiFi association.
  // Now that the network is up (STA only — AP mode keeps the splash
  // until DispatchBootPath repaints the provisioning UI), start the
  // boot spinner. It performs a parallel kFull clear so the splash row
  // disappears in the same pass that paints the spinner's first frame.
  if (!ctx.wifi->is_ap_mode()) {
    btclock::StartBootSpinner(ctx);
  }
  btclock::InitScreenManager(ctx);
  // DispatchBootPath kicks off the data sources but no longer blocks
  // for the first snapshot — that wait used to gate InitControlApi
  // behind ~3-30 s of "wait for first blockheight push", which made
  // the device feel HTTP-dead until the screen lit up. We start the
  // control API as soon as the source tasks are spawned so /api/* is
  // responsive while the first data is still in flight.
  btclock::DispatchBootPath(ctx);
  btclock::InitControlApi(ctx);
  // Tail of WireDataSources: blocking wait for first blockheight,
  // first render, button bring-up. Idempotent in AP mode (skips when
  // there's no hub).
  btclock::FinishWiringDataSources(ctx);
  btclock::InitMdns(ctx);

  btclock::RunEventLoop(ctx);
}
