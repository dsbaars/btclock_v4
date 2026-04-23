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
    // The array itself is static ("") until a screen-text mirror is
    // wired up; this only governs length.
    size_t num_screens = 0;
    // Human-readable hardware label, e.g. "Rev B", "V8".
    std::string hw_name;
    // Optional frontlight hook. Nullptr on boards without a PCA9685
    // backlight (Rev A, V8); /api/frontlight/* then responds 503.
    FrontlightIface* frontlight = nullptr;
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
  };
  void PublishStatus(const LiveStatus& status);

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
  static esp_err_t TrampolineNotImplemented(httpd_req_t* req);
  static esp_err_t TrampolineOptions(httpd_req_t* req);

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

  bool PostCommand(const ControlCommand& cmd);
  static void ApplyCors(httpd_req_t* req);

  Config cfg_;
  httpd_handle_t server_ = nullptr;
  QueueHandle_t cmd_queue_ = nullptr;

  // Live-status snapshot. Lightweight — one int + short string — so a
  // single std::mutex is more than enough.
  mutable std::mutex status_mu_;
  LiveStatus status_;
};

}  // namespace btclock
