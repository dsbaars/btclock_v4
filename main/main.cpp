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
#include "app/event_loop.hpp"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
      ESP_LOGE("boot", "coredump from previous run present (%u bytes)",
               static_cast<unsigned>(size));
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
  btclock::InitNetwork(ctx);
  btclock::InitScreenManager(ctx);
  btclock::DispatchBootPath(ctx);
  btclock::InitControlApi(ctx);
  btclock::InitMdns(ctx);

  btclock::RunEventLoop(ctx);
}
