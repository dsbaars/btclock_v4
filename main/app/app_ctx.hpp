// Shared application context.
//
// Owns every subsystem handle that used to live as a file-scope static
// or stack local in main.cpp. The init_* TUs under app/boot/ populate
// fields on this struct; the event loop in app/event_loop.cpp reads
// them. Keeping it behaviour-free keeps lifetimes explicit and makes
// the boot flow easy to reason about: construct, run inits in order,
// run event loop.
//
// Pointers: owning handles use std::unique_ptr (nullable so we can
// represent the "AP mode / no STA services" branch without a second
// struct). Values constructed unconditionally at boot live inline.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/screen_manager.hpp"
#include "bh1750.hpp"
#include "board/board.hpp"
#include "buttons.hpp"
#include "data_core/hub.hpp"
#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "i2c_bus.hpp"
#include "mcp23017.hpp"
#include "pca9685.hpp"
#include "prefs.hpp"

// Forward declarations — avoid dragging the full headers into every TU
// that includes app_ctx.hpp.
namespace btclock {
class BtclockDataSource;
class ControlServer;
class SseServer;
class DnsHijack;
class FrontlightController;
class LightSensor;
class NetworkLedWatchdog;
class OutageWatchdog;
class ProvisioningServer;
class Wifi;
namespace nostr {
class NostrDataSource;
class RelayClient;
class SubscriptionManager;
class ZapListener;
}  // namespace nostr
}  // namespace btclock

namespace btclock {

// Forward-declared so app_ctx.hpp doesn't need control_server.hpp.
// Full definitions in main/app/boot/adapters.hpp.
struct FrontlightAdapter;
struct LedsAdapter;
struct DndAdapter;
struct LightSensorAdapter;
struct TimerAdapter;

struct AppCtx {
  AppCtx();
  ~AppCtx();
  AppCtx(const AppCtx&) = delete;
  AppCtx& operator=(const AppCtx&) = delete;

  // Hardware — I2C + port expanders + PWM + light sensor.
  std::optional<I2cBus> i2c;
  std::optional<Mcp23017> mcp;   // always present
  std::optional<Mcp23017> mcp2;  // V8 only
  std::optional<Pca9685> pca;    // boards with frontlight
  std::unique_ptr<FrontlightController> frontlight;
  std::optional<Bh1750> bh;
  std::unique_ptr<LightSensor> light_sensor;

  // Panels + framebuffer storage. fb_storage lives inline because it
  // needs a fixed address passed by reference into Render().
  std::optional<EpdBus> epd_bus;
  std::array<std::unique_ptr<EpdPanel>, btclock::board::kNumPanels> panels;
  // Framebuffer storage must be static so its address stays valid for
  // the life of the program. Defined in app_ctx_storage.cpp (one .o)
  // so multiple TUs can take its address.
  static uint8_t (&fb_storage())[btclock::board::kNumPanels][16 * 296];

  // Fonts — asset loader + role accessors (Agent B extends this).
  AppFonts fonts;

  // WiFi + optional AP-mode portal.
  std::unique_ptr<Wifi> wifi;
  std::unique_ptr<ProvisioningServer> portal;
  std::unique_ptr<DnsHijack> dns;
  std::string ap_ssid;
  std::string ap_pw;
  std::string sta_ssid;  // remembered for the debug screen
  std::unique_ptr<OutageWatchdog> outage_watchdog;
  std::unique_ptr<NetworkLedWatchdog> network_led_watchdog;

  // Data pipeline — the hub aggregates every DataSource; the screen
  // manager owns rotation + render bookkeeping.
  std::unique_ptr<DataHub> hub;
  // Non-owning back-ref to the v2 WS source the hub holds in its
  // sources_ vector. Set by sources.cpp at AddSource time so the
  // on_screens_changed hook can refresh the per-currency subscription
  // list without poking through DataHub internals. Cleared in
  // sources.cpp::WireDataSources when AP-mode skips the source bring-up.
  BtclockDataSource* btclock_ws = nullptr;
  // Non-owning back-ref to the Nostr data source when dataSource=2.
  // Set in MaybeAddNostrSource right after AddSource so InitZapListener
  // (which runs later in boot) can decide whether to share that source's
  // RelayClient + SubscriptionManager instead of spawning a second WSS.
  // Sharing collapses ~30+ KB of internal SRAM (a second 12 KB WS task
  // stack + 8 KB rx buffer + mbedTLS context) and the matching
  // largest-block fragmentation that was silently breaking the EPD
  // render path. See bd btclock_v4-17r and the URL-match gate in
  // init_zap_listener.cpp::ShouldShareNostrRelay.
  nostr::NostrDataSource* nostr_source = nullptr;
  // Non-owning back-ref to the mempool+kraken source when dataSource=1.
  // Used by /api/status to surface the two channels' live connection
  // state separately (price → kraken, blocks → mempool); null on every
  // other dataSource path. Forward-declared instead of #included to
  // keep app_ctx.hpp's include footprint small.
  class MempoolKrakenSource* mempool_kraken = nullptr;
  std::unique_ptr<ScreenManager> sm;
  std::vector<std::string> currencies;

  // Buttons — queue + reader.
  QueueHandle_t button_q = nullptr;
  std::unique_ptr<ButtonReader> buttons;
  TaskHandle_t main_task = nullptr;

  // Zap-listener stack + atomic flags feeding the on-zap callback.
  std::unique_ptr<nostr::RelayClient> zap_relay;
  std::unique_ptr<nostr::SubscriptionManager> zap_subs;
  std::unique_ptr<nostr::ZapListener> zap_listener;
  // Last-known zap recipient pubkey. RefreshZapListenerSettings
  // compares against this on every PATCH so we only Stop()+Start()
  // the listener when the pubkey actually changed (LED/frontlight
  // toggles touch only the atomics below).
  std::string zap_pubkey_current;
  std::atomic<bool> flash_on_zap_enabled{true};
  std::atomic<bool> flash_frontlight_on_zap_enabled{false};
  std::atomic<bool> zap_notify_screen_enabled{true};
  std::atomic<bool> zap_screen_auto_restore{true};
  std::atomic<bool> zap_notify_pending{false};

  // Control API + SSE fan-out. Adapters are owned here so their
  // lifetime spans the whole run (ControlServer holds raw pointers
  // to them). Adapter structs declared in app/boot/adapters.hpp.
  std::unique_ptr<ControlServer> ctrl;
  std::unique_ptr<SseServer> sse;
  std::unique_ptr<FrontlightAdapter> fl_adapter;
  std::unique_ptr<LedsAdapter> leds_adapter;
  std::unique_ptr<DndAdapter> dnd_adapter;
  std::unique_ptr<LightSensorAdapter> light_sensor_adapter;
  std::unique_ptr<TimerAdapter> timer_adapter;
};

}  // namespace btclock
