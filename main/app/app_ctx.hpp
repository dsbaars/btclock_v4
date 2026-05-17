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
#include "epd/panel.hpp"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
#if BTCLOCK_HAS_FRONTLIGHT
class FrontlightController;
#endif
#if BTCLOCK_HAS_BH1750
class LightSensor;
#endif
class NetworkLedWatchdog;
class OutageWatchdog;
class ProvisioningServer;
class Wifi;
namespace nostr {
class NostrDataSource;
class RelayClient;
class SubscriptionManager;
class ZapListener;
class ZapIdLru;
}  // namespace nostr
namespace nwc {
class NwcClient;
class NotificationQueue;
}  // namespace nwc
}  // namespace btclock

namespace btclock {

// Forward-declared so app_ctx.hpp doesn't need control_server.hpp.
// Full definitions in main/app/boot/adapters.hpp.
#if BTCLOCK_HAS_FRONTLIGHT
struct FrontlightAdapter;
#endif
struct LedsAdapter;
struct DndAdapter;
#if BTCLOCK_HAS_BH1750
struct LightSensorAdapter;
#endif
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
#if BTCLOCK_HAS_FRONTLIGHT
  std::optional<Pca9685> pca;    // boards with frontlight
  std::unique_ptr<FrontlightController> frontlight;
#endif
#if BTCLOCK_HAS_BH1750
  std::optional<Bh1750> bh;
  std::unique_ptr<LightSensor> light_sensor;
#endif

  // Panels + framebuffer storage. fb_storage lives inline because it
  // needs a fixed address passed by reference into Render().
  std::optional<EpdBus> epd_bus;
  std::array<std::unique_ptr<epd::IEpdPanel>, btclock::board::kNumPanels>
      panels;
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
  // Non-owning back-refs to the Nostr data sources — one per configured
  // relay when dataSource=2. Set in MaybeAddNostrSource right after each
  // AddSource so InitZapListener (which runs later in boot) can find a
  // matching data-source URL and share its RelayClient +
  // SubscriptionManager via NIP-01 multi-sub. Sharing collapses ~13 KB
  // of internal SRAM + ~24 KB PSRAM per extra relay (measured Rev B
  // 2026-05-08); without it the largest-free-block ceiling silently
  // broke the EPD render path on long-running multi-relay devices. See
  // the URL-match gate in init_zap_listener.cpp::ShouldShareNostrRelay
  // and the per-relay enumeration walk in /api/status connectionStatus.
  std::vector<nostr::NostrDataSource*> nostr_sources;
  // Non-owning back-ref to the mempool+kraken source when dataSource=1.
  // Used by /api/status to surface the two channels' live connection
  // state separately (price → kraken, blocks → mempool); null on every
  // other dataSource path. Forward-declared instead of #included to
  // keep app_ctx.hpp's include footprint small.
  class MempoolKrakenSource* mempool_kraken = nullptr;
  std::unique_ptr<ScreenManager> sm;
  std::vector<std::string> currencies;
  // Catalogue of currency codes the *upstream* publishes. Default-filled
  // from catalogs::kAvailableCurrencies in InitScreenManager so the
  // settings drop-down has a sensible list before WireDataSources runs;
  // overwritten by FetchAvailableCurrencies on dataSource=0/2 when the
  // GET /api/v2/currencies probe lands. dataSource=1 (mempool+kraken)
  // keeps the static catalogue — Kraken's currency set is independent of
  // the v2 API.
  std::vector<std::string> available_currencies;

  // Buttons — queue + reader.
  QueueHandle_t button_q = nullptr;
  std::unique_ptr<ButtonReader> buttons;
  TaskHandle_t main_task = nullptr;

  // Zap-listener stack + atomic flags feeding the on-zap callback. The
  // three vectors are parallel and one entry deep per *dedicated* WSS
  // (i.e. a relay where the zap listener does NOT ride a sibling
  // NostrDataSource). When a listener shares a data source's
  // SubscriptionManager, only zap_listeners gets an entry — zap_relays
  // and zap_subs stay short. zap_listeners is the primary list /api/
  // status walks alongside nostr_sources to enumerate every live relay.
  std::vector<std::unique_ptr<nostr::RelayClient>> zap_relays;
  std::vector<std::unique_ptr<nostr::SubscriptionManager>> zap_subs;
  std::vector<std::unique_ptr<nostr::ZapListener>> zap_listeners;
  // Shared event-id LRU for multi-relay zap dedup. Each ZapListener's
  // bound on-zap callback consults this before firing the snapshot
  // patch + screen-overlay notification, so two relays delivering the
  // same kind-9735 receipt only surface one notification. Heap-allocated
  // because ZapIdLru is forward-declared here (full definition lives in
  // components/nostr).
  std::unique_ptr<nostr::ZapIdLru> zap_id_lru;
  // Last-known zap recipient pubkey list. RefreshZapListenerSettings
  // compares against this on every PATCH so we only Stop()+Start()
  // the listener when the recipient set actually changed (LED /
  // frontlight toggles touch only the atomics below). Order matters —
  // it's the order we emit them in the REQ filter, and a user reorder
  // counts as a change.
  std::vector<std::string> zap_pubkeys_current;
  std::atomic<bool> flash_on_zap_enabled{true};
  std::atomic<bool> flash_frontlight_on_zap_enabled{false};
  std::atomic<bool> zap_notify_screen_enabled{true};
  std::atomic<bool> zap_screen_auto_restore{true};
  std::atomic<bool> zap_notify_pending{false};

  // NWC (NIP-47) wallet — at most one client active at a time. The
  // dedicated WSS does NOT share with data-source relays: NWC relays
  // are almost always wallet-operator hosts (e.g. relay.getalby.com)
  // distinct from the pricing/zap relays, and the message stream is
  // pubkey-filtered against the wallet service rather than the
  // user pubkey, so the multiplexing the zap path uses doesn't apply.
  std::unique_ptr<nostr::RelayClient> nwc_relay;
  std::unique_ptr<nostr::SubscriptionManager> nwc_subs;
  std::unique_ptr<nwc::NwcClient> nwc_client;
  // Bounded queue + dedicated worker for kind 23197/23196 payment
  // notifications. The decrypt + cJSON parse for these used to run
  // synchronously on the esp_websocket_client RX-task (~3-4 KiB
  // stack) and overflowed it on first arrival. The hot path now
  // copies the encrypted envelope into the queue and returns
  // immediately; the worker (8 KiB stack) does the heavy work.
  // bd btclock_v4-lwf.9.
  std::unique_ptr<nwc::NotificationQueue> nwc_notif_queue;
  TaskHandle_t nwc_notif_worker = nullptr;
  // ESP timer firing the periodic get_balance poll. Owned here so the
  // settings PATCH path can re-prime the interval without re-entering
  // the boot init. nullptr when NWC is disabled.
  void* nwc_refresh_timer = nullptr;
  // True iff InitNwc successfully constructed an NwcClient. Used by the
  // event-loop to decide whether to drain the payment-notification
  // pending flag; cheaper than nulling-out the unique_ptrs on disable.
  std::atomic<bool> nwc_enabled{false};
  // Set to true by the on-payment callback when a kind 23196/23197
  // notification arrives and `nwcFlashOnPay` is on. The event-loop
  // picks it up on the next wake, calls sm.SetNwcPaymentNotify(), and
  // (when the gate is on) fires the LED+frontlight pulse — same shape
  // as zap_notify_pending.
  std::atomic<bool> nwc_notify_pending{false};
  // Set to true by the esp_timer poll callback (~`nwcRefreshSecs`).
  // The event-loop drains the flag on the main task and runs the
  // heavy NIP-44 encrypt + schnorr sign there — the esp_timer task
  // ships with a ~3.5 KiB stack which `RequestGetBalance` blew past,
  // tripping `***ERROR*** A stack overflow in task esp_timer`.
  // bd btclock_v4-lwf.6.
  std::atomic<bool> nwc_refresh_pending{false};
  // Master gate. PATCH-flipping this on requires reboot (boot path
  // owns the construction); flipping off pauses dispatch but keeps the
  // WSS up — letting the user re-enable without a full re-init.
  std::atomic<bool> nwc_flash_on_payment_enabled{true};
  // Soft auto-restore latch matching the zap overlay's scrnRestoreZap
  // behaviour. PATCHed via /api/settings; defaults to true so the
  // overlay clears itself after the 8 s window.
  std::atomic<bool> nwc_notify_auto_restore{true};

  // OTA "UPDATE!" overlay render-on-main-task handoff. The pre-flash
  // hook fires on the ota_auto worker task (auto-update) or the httpd
  // worker (push-OTA); both are not the task that owns font.cpp's
  // glyph_buf scratch. Rendering directly from those tasks tripped
  // the single-thread invariant and aborted the process. Set
  // ota_overlay_render_pending=true + xTaskNotifyGive(main_task), then
  // wait on ota_overlay_rendered_sem so the hook returns only after
  // the main loop has painted the overlay. Semaphore is created at
  // AppCtx construction (binary, starts not-signalled).
  std::atomic<bool> ota_overlay_render_pending{false};
  SemaphoreHandle_t ota_overlay_rendered_sem = nullptr;

  // Control API + SSE fan-out. Adapters are owned here so their
  // lifetime spans the whole run (ControlServer holds raw pointers
  // to them). Adapter structs declared in app/boot/adapters.hpp.
  std::unique_ptr<ControlServer> ctrl;
  std::unique_ptr<SseServer> sse;
#if BTCLOCK_HAS_FRONTLIGHT
  std::unique_ptr<FrontlightAdapter> fl_adapter;
#endif
  std::unique_ptr<LedsAdapter> leds_adapter;
  std::unique_ptr<DndAdapter> dnd_adapter;
#if BTCLOCK_HAS_BH1750
  std::unique_ptr<LightSensorAdapter> light_sensor_adapter;
#endif
  std::unique_ptr<TimerAdapter> timer_adapter;
};

}  // namespace btclock
