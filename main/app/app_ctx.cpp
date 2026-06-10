// Out-of-line ctor/dtor + static framebuffer storage for AppCtx.
//
// AppCtx holds std::unique_ptr<T> members for several forward-declared
// types (ControlServer, FrontlightController, ZapListener, …). Those
// unique_ptrs need the full destructor visible wherever they are
// destroyed. Putting the destructor here — with every concrete header
// included — keeps every other TU free of the heavyweight includes.

#include "app/app_ctx.hpp"

#include "app/boot/adapters.hpp"
#include "app/network_coordinator.hpp"
#include "control_server.hpp"
#include "esp_timer.h"
#if BTCLOCK_HAS_FRONTLIGHT
#include "io/frontlight_controller.hpp"
#endif
#if BTCLOCK_HAS_BH1750
#include "io/light_sensor.hpp"
#endif
#include "io/network_led_watchdog.hpp"
#include "io/wifi_guard.hpp"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"
#include "nostr/zap_id_lru.hpp"
#include "nostr/zap_listener.hpp"
#include "nwc/client.hpp"
#include "provisioning_server.hpp"
#include "sse_server.hpp"
#include "wifi.hpp"

namespace btclock {

AppCtx::AppCtx() {
  // Binary semaphore the main loop posts after rendering the OTA
  // overlay. Constructed in not-signalled state so the first
  // xSemaphoreTake() in the pre-flash hook blocks until the main
  // loop's drain path posts.
  ota_overlay_rendered_sem = xSemaphoreCreateBinary();
}

AppCtx::~AppCtx() {
  if (ota_overlay_rendered_sem != nullptr) {
    vSemaphoreDelete(ota_overlay_rendered_sem);
    ota_overlay_rendered_sem = nullptr;
  }
  // NWC refresh timer outlives the NwcClient via void*; the boot path
  // creates it via esp_timer_create and stores the handle on AppCtx.
  // Destroying here keeps the lifetime symmetric with the unique_ptrs.
  if (nwc_refresh_timer != nullptr) {
    auto* h = static_cast<esp_timer_handle_t>(nwc_refresh_timer);
    esp_timer_stop(h);
    esp_timer_delete(h);
    nwc_refresh_timer = nullptr;
  }
}

uint8_t (&AppCtx::fb_storage()) [btclock::board::kNumPanels][16 * 296] {
  static uint8_t storage[btclock::board::kNumPanels][16 * 296];
  return storage;
}

}  // namespace btclock
