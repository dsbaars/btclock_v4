// BTClock ESP-IDF C++ PoC — orchestrator.
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
// Board variant comes from -DPOC_BOARD=REV_A|REV_B|V8 at build time.

#include "app/app_ctx.hpp"
#include "app/boot/init_boot_leds.hpp"
#include "app/boot/init_boot_path.hpp"
#include "app/boot/init_control_api.hpp"
#include "app/boot/init_hardware.hpp"
#include "app/boot/init_mdns.hpp"
#include "app/boot/init_network.hpp"
#include "app/boot/init_panels.hpp"
#include "app/boot/init_screen_manager.hpp"
#include "app/boot/init_storage.hpp"
#include "app/event_loop.hpp"

extern "C" void app_main() {
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
