// HTTP control-API server — the runtime counterpart to the provisioning
// portal. Started in STA mode after WiFi comes up. Mirrors the endpoints
// exposed by the production firmware (src/lib/net/webserver/*) so the
// existing WebUI in data/ can drive the IDF PoC unchanged.
//
// The endpoint surface is defined by data/static/swagger.yml. Endpoints
// whose backing subsystem has not yet been ported to IDF (DND, lights,
// frontlight, OTA, full settings CRUD) are registered as 501 stubs so
// clients see a clear "not implemented" error rather than a 404 that
// makes it look like the route is missing entirely.
//
// Threading: all request handlers run on the esp_http_server worker
// task. Subsystems safe to touch directly from that task — DataHub (has
// its own mutex), esp_wifi_*, esp_system, esp_timer, NVS — are called
// inline. Anything that mutates main-loop state (screen index, screen
// manager flags, EPD refresh, esp_restart) goes through the command
// queue so the main task can process it on its next iteration without
// us having to synchronise ScreenManager.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "data_core/hub.hpp"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wifi.hpp"

namespace btclock {

class SseServer;  // components/webserver/include/sse_server.hpp


// Minimal abstract surface the control server calls to drive the
// per-board frontlight controller. Kept as a pure-virtual interface so
// the webserver component doesn't have to depend on `main/`, where the
// concrete `FrontlightController` lives; main wires up a thin adapter
// (see main.cpp). Boards without a frontlight (Rev A, V8) pass
// `nullptr` and the /api/frontlight/* handlers respond 503.
class FrontlightIface {
 public:
  struct Status {
    bool enabled;
    uint16_t current_duty;
    uint16_t target_duty;
    uint16_t configured_brightness;
    uint32_t lux_threshold;
    bool ambient_auto_off;
  };
  virtual ~FrontlightIface() = default;
  virtual void On() = 0;
  virtual void Off() = 0;
  virtual void Flash() = 0;
  virtual void SetBrightness(uint16_t duty) = 0;
  virtual Status GetStatus() const = 0;
};

// Do-Not-Disturb surface. The webserver component stays free of the
// dnd component (and its NVS + mutex deps) by routing all DND reads
// and writes through this interface. main.cpp wires it to the
// process-wide btclock::dnd::Dnd singleton. Nullptr -> /api/dnd/*
// endpoints respond 503 and /api/status emits the off/false stub.
class DndIface {
 public:
  struct Status {
    bool enabled = false;         // manual "force on now" flag
    bool time_enabled = false;    // schedule gate
    uint8_t start_hour = 0;
    uint8_t start_minute = 0;
    uint8_t end_hour = 0;
    uint8_t end_minute = 0;
    bool active = false;          // current wall-clock query
  };
  virtual ~DndIface() = default;
  virtual Status GetStatus() const = 0;
  virtual void SetEnabled(bool enabled) = 0;
};

// Screen-rotation timer control — lets /api/action/pause and
// /api/action/timer_restart reach the main-loop ScreenManager without
// the webserver component having to depend on main/. Concrete
// adapter in main.cpp forwards to the ScreenManager instance.
class TimerIface {
 public:
  virtual ~TimerIface() = default;
  virtual bool IsPaused() const = 0;
  virtual void SetPaused(bool paused) = 0;
  virtual void Restart() = 0;
};

// NeoPixel-side counterpart to FrontlightIface. Keeps the webserver
// component independent of `main/app/led_controller.hpp` (which drags
// FreeRTOS + RMT + NVS into every TU that includes it). `main.cpp`
// wires up a thin adapter that forwards these calls to the controller's
// namespace-level functions.
//
// Semantics mirror the old firmware's /api/lights surface:
//   - GetStatus: read the current per-pixel mirror + master prefs.
//   - SetSolidColor: paint every pixel the same colour; 0 = off.
//   - SetPixels: per-pixel RGB; `count` clamped to the strip width.
//   - SetDisabled: global mute flag (persisted to NVS).
//   - TriggerIdentify: fire the identify effect (rapid multi-colour).
class LedsIface {
 public:
  struct Status {
    uint8_t brightness = 0;
    uint32_t block_flash_color = 0;
    bool disabled = false;
    bool flash_on_update = false;
    // Per-pixel colour mirror. `pixel_count` is the filled prefix.
    uint32_t pixels[8] = {0};
    uint32_t pixel_count = 0;
  };
  virtual ~LedsIface() = default;
  virtual Status GetStatus() const = 0;
  virtual void SetSolidColor(uint32_t rgb) = 0;
  virtual void SetPixels(const uint32_t* rgb_array, uint32_t count) = 0;
  virtual void SetDisabled(bool disabled) = 0;
  virtual void SetBrightness(uint8_t value) = 0;
  virtual void SetBlockFlashColor(uint32_t rgb) = 0;
  virtual void TriggerIdentify() = 0;
};

// Commands the HTTP task posts to the main task. `arg` is command-
// specific (e.g. slot index for kShowScreen). Keep this trivially
// copyable — we xQueueSend it by value.
struct ControlCommand {
  enum class Kind : uint8_t {
    kFullRefresh,
    kIdentify,
    kRestart,
    kShowScreen,      // arg_i = slot index
    kShowCurrency,    // arg_s = currency code
    kNextScreen,
    kPrevScreen,
    kStopDataSources,
    kRestartDataSources,
  };
  Kind kind;
  int32_t arg_i = 0;
  char arg_s[16] = {0};  // short enough for currency codes ("USD"+"\0")
};

class ControlServer {
 public:
  struct Config {
    Wifi* wifi = nullptr;
    DataHub* hub = nullptr;
    // Non-empty list of active currency codes. The control server uses
    // it to resolve /api/show/currency?c=<code> against /api/status's
    // `actCurrencies` and to size price arrays in /api/status.
    std::vector<std::string> currencies;
    // Number of EPD panels — drives the size of /api/status `data`.
    // The array contents come from ScreenManager via LiveStatus's
    // `panel_texts`; this keeps the length pinned even before the
    // first render has populated that mirror.
    size_t num_screens = 0;
    // Human-readable hardware label, e.g. "Rev B", "V8".
    std::string hw_name;
    // Optional frontlight hook. Nullptr on boards without a PCA9685
    // backlight (Rev A, V8); /api/frontlight/* then responds 503.
    FrontlightIface* frontlight = nullptr;
    // NeoPixel control surface. Nullptr if the LED subsystem failed to
    // initialise — /api/lights/* will 503 in that case. Every BTClock
    // board ships with WS2812B strips so this is expected to be set.
    LedsIface* leds = nullptr;
    // DND control surface. Nullptr -> /api/dnd/* returns 503 and the
    // /api/status `dnd` block reports the inactive stub.
    DndIface* dnd = nullptr;
    // Screen-rotation timer. Nullptr -> /api/action/pause and
    // /api/action/timer_restart respond 503.
    TimerIface* timer = nullptr;

    // Full rotatable-screen catalogue used by /api/settings. Order
    // here is the fallback rotation order (the WebUI shows this as
    // the default before the user customises). Id/name map entries
    // mirror ScreenMapping in the old firmware.
    struct ScreenEntry {
      int id;
      std::string name;
    };
    std::vector<ScreenEntry> screens_catalog;
    // Fonts + pools + currencies the renderer and data-source layer
    // support. Drives the "availableFonts" / "availablePools" /
    // "availableCurrencies" arrays in GET /api/settings so the WebUI
    // can populate its dropdowns. `currencies` above is the *active*
    // subset; `available_currencies` is the full set.
    std::vector<std::string> available_fonts;
    std::vector<std::string> available_pools;
    std::vector<std::string> available_currencies;
  };

  explicit ControlServer(Config cfg);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Start listening on port 80. Must be called after WiFi STA is up
  // (the server binds to all interfaces; calling it in AP mode would
  // collide with ProvisioningServer).
  esp_err_t Start();

  // Pop one pending command into `out`. Non-blocking. The main task
  // calls this once per event-loop iteration.
  bool TryPopCommand(ControlCommand* out);

  // Snapshot of live status emitted by the main task so /api/status
  // responses reflect the actual screen state without poking the
  // ScreenManager from the HTTP task. Updated on every render; read
  // atomically under a small mutex.
  struct LiveStatus {
    int32_t current_slot = 0;
    int32_t slot_count = 1;
    bool timer_running = true;
    std::string currency;  // "" for block screen
    // Per-panel text mirror for /api/status `data[]`. Length should be
    // `num_screens` when populated. Empty means the first render hasn't
    // happened yet; HandleStatus falls back to empty strings then.
    std::vector<std::string> panel_texts;
  };
  void PublishStatus(const LiveStatus& status);

  // Attach an SSE server for live broadcasts. Non-owning — the caller
  // (main.cpp) keeps the SseServer alive for the ControlServer's
  // lifetime. When set, state-changing handlers push an updated
  // `status` event after applying their mutation, and callers can
  // force a broadcast with `BroadcastStatus()` after a DataHub
  // snapshot update lands.
  void AttachSse(SseServer* sse) { sse_ = sse; }

  // Emit the current status JSON as an SSE `status` event to every
  // connected client. Cheap no-op if no SSE server is attached or no
  // clients are connected. Safe to call from any task.
  void BroadcastStatus();

 private:
  static esp_err_t TrampolineStatus(httpd_req_t* req);
  static esp_err_t TrampolineSystemStatus(httpd_req_t* req);
  static esp_err_t TrampolineFullRefresh(httpd_req_t* req);
  static esp_err_t TrampolineIdentify(httpd_req_t* req);
  static esp_err_t TrampolineRestart(httpd_req_t* req);
  static esp_err_t TrampolineShowScreen(httpd_req_t* req);
  static esp_err_t TrampolineShowCurrency(httpd_req_t* req);
  static esp_err_t TrampolineScreenNext(httpd_req_t* req);
  static esp_err_t TrampolineScreenPrev(httpd_req_t* req);
  static esp_err_t TrampolineStopDataSources(httpd_req_t* req);
  static esp_err_t TrampolineRestartDataSources(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightOn(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightOff(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightFlash(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightStatus(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightBrightness(httpd_req_t* req);
  static esp_err_t TrampolineWifiTxPower(httpd_req_t* req);
  static esp_err_t TrampolineUploadWebui(httpd_req_t* req);
  static esp_err_t TrampolineLightsStatus(httpd_req_t* req);
  static esp_err_t TrampolineLightsColor(httpd_req_t* req);
  static esp_err_t TrampolineLightsOff(httpd_req_t* req);
  static esp_err_t TrampolineLightsSet(httpd_req_t* req);
  static esp_err_t TrampolineSettingsGet(httpd_req_t* req);
  static esp_err_t TrampolineSettingsPatch(httpd_req_t* req);
  static esp_err_t TrampolineDndStatus(httpd_req_t* req);
  static esp_err_t TrampolineDndEnable(httpd_req_t* req);
  static esp_err_t TrampolineDndDisable(httpd_req_t* req);
  static esp_err_t TrampolineActionPause(httpd_req_t* req);
  static esp_err_t TrampolineActionTimerRestart(httpd_req_t* req);
  static esp_err_t TrampolineNotImplemented(httpd_req_t* req);
  static esp_err_t TrampolineOptions(httpd_req_t* req);
  static esp_err_t TrampolineStatic(httpd_req_t* req);

  esp_err_t HandleStatus(httpd_req_t* req);
  esp_err_t HandleSystemStatus(httpd_req_t* req);
  esp_err_t HandleFullRefresh(httpd_req_t* req);
  esp_err_t HandleIdentify(httpd_req_t* req);
  esp_err_t HandleRestart(httpd_req_t* req);
  esp_err_t HandleShowScreen(httpd_req_t* req);
  esp_err_t HandleShowCurrency(httpd_req_t* req);
  esp_err_t HandleScreenNext(httpd_req_t* req);
  esp_err_t HandleScreenPrev(httpd_req_t* req);
  esp_err_t HandleStopDataSources(httpd_req_t* req);
  esp_err_t HandleRestartDataSources(httpd_req_t* req);
  esp_err_t HandleFrontlightOn(httpd_req_t* req);
  esp_err_t HandleFrontlightOff(httpd_req_t* req);
  esp_err_t HandleFrontlightFlash(httpd_req_t* req);
  esp_err_t HandleFrontlightStatus(httpd_req_t* req);
  esp_err_t HandleFrontlightBrightness(httpd_req_t* req);
  esp_err_t HandleWifiTxPower(httpd_req_t* req);
  esp_err_t HandleUploadWebui(httpd_req_t* req);
  esp_err_t HandleLightsStatus(httpd_req_t* req);
  esp_err_t HandleLightsColor(httpd_req_t* req);
  esp_err_t HandleLightsOff(httpd_req_t* req);
  esp_err_t HandleLightsSet(httpd_req_t* req);
  esp_err_t HandleSettingsGet(httpd_req_t* req);
  esp_err_t HandleSettingsPatch(httpd_req_t* req);
  esp_err_t HandleDndStatus(httpd_req_t* req);
  esp_err_t HandleDndEnable(httpd_req_t* req);
  esp_err_t HandleDndDisable(httpd_req_t* req);
  esp_err_t HandleActionPause(httpd_req_t* req);
  esp_err_t HandleActionTimerRestart(httpd_req_t* req);
  esp_err_t HandleStatic(httpd_req_t* req);

  bool PostCommand(const ControlCommand& cmd);
  static void ApplyCors(httpd_req_t* req);

  // Render the current /api/status JSON as a string. Single source of
  // truth — both the REST handler and SSE broadcasts use this so a
  // new field added to one automatically surfaces on the other.
  // Empty string on OOM, which the caller surfaces as a 500.
  std::string BuildStatusJson() const;

  Config cfg_;
  httpd_handle_t server_ = nullptr;
  QueueHandle_t cmd_queue_ = nullptr;
  SseServer* sse_ = nullptr;  // non-owning; nullable

  // Live-status snapshot. Lightweight — one int + short string — so a
  // single std::mutex is more than enough.
  mutable std::mutex status_mu_;
  LiveStatus status_;
};

}  // namespace btclock
