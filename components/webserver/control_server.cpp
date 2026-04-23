#include "control_server.hpp"
#include "control_validators.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "littlefs.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "ctrl-api";

// Auth — the old Arduino server gates every route on an HTTP Basic
// check when httpAuthEnabled is true. Porting that requires the full
// settings subsystem (otaPass/httpAuthPass), which isn't in the IDF
// port yet. Follow-up work should add a Require* helper at the top of
// each handler here; for now every endpoint is open. Do NOT expose the
// device to the public internet until this lands.

// Response helpers -----------------------------------------------------

constexpr const char* kJsonType = "application/json";

esp_err_t SendJson(httpd_req_t* req, const std::string& body) {
  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t SendJsonChar(httpd_req_t* req, char* body) {
  // cJSON_PrintUnformatted returns a malloc'd buffer the caller must
  // free. Always wrap the send in a lambda so the free runs even if
  // httpd_resp_send fails.
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  const esp_err_t rc = SendJson(req, body);
  free(body);
  return rc;
}

esp_err_t SendEmptyOk(httpd_req_t* req) {
  // Old firmware's HTTP_OK helper sends an empty body; match that.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t SendNotImplemented(httpd_req_t* req, const char* tracking) {
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char body[160];
  std::snprintf(body, sizeof(body),
                "{\"error\":\"not_implemented\",\"tracking\":\"%s\"}",
                tracking ? tracking : "pending");
  return httpd_resp_send(req, body, strlen(body));
}

// --- Query-string helpers -------------------------------------------
// esp_http_server doesn't give us a getparam-or-null like ESPAsync's
// AsyncWebServerRequest::getParam(), so the endpoints parse the query
// themselves. Good enough at this scale (all control endpoints take
// 0-1 parameters) without pulling in a query-string library.

bool QueryParam(httpd_req_t* req, const char* key,
                char* out, size_t out_size) {
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 256) return false;
  char qbuf[256];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(qbuf, key, out, out_size) == ESP_OK;
}

// --- Misc -----------------------------------------------------------

int CurrentRssi() {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
  return 0;
}

}  // namespace

// --- ControlServer --------------------------------------------------

ControlServer::ControlServer(Config cfg) : cfg_(std::move(cfg)) {
  status_.current_slot = 0;
  status_.slot_count = cfg_.currencies.empty() ? 1
                       : static_cast<int32_t>(1 + 2 * cfg_.currencies.size());
  status_.timer_running = true;
  status_.currency = "";
}

ControlServer::~ControlServer() {
  if (server_) httpd_stop(server_);
  if (cmd_queue_) vQueueDelete(cmd_queue_);
}

void ControlServer::PublishStatus(const LiveStatus& status) {
  std::lock_guard<std::mutex> lk(status_mu_);
  status_ = status;
}

bool ControlServer::TryPopCommand(ControlCommand* out) {
  if (!cmd_queue_ || !out) return false;
  return xQueueReceive(cmd_queue_, out, 0) == pdTRUE;
}

bool ControlServer::PostCommand(const ControlCommand& cmd) {
  if (!cmd_queue_) return false;
  return xQueueSend(cmd_queue_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

void ControlServer::ApplyCors(httpd_req_t* req) {
  // Permissive CORS matches the production firmware, which sets
  // Access-Control-Allow-Origin: * globally (lib/net/webserver/webserver.cpp).
  // Tightening this belongs in the same follow-up that adds HTTP Basic auth.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                     "GET, POST, PATCH, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                     "Content-Type, Authorization");
}

esp_err_t ControlServer::TrampolineOptions(httpd_req_t* req) {
  ApplyCors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t ControlServer::Start() {
  cmd_queue_ = xQueueCreate(8, sizeof(ControlCommand));
  if (!cmd_queue_) return ESP_ERR_NO_MEM;

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  // Bump handler cap — we register ~20 endpoints across the control
  // surface and stubs; HTTPD_DEFAULT_CONFIG's 8 is not enough.
  cfg.max_uri_handlers = 32;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 8192;
  cfg.lru_purge_enable = true;

  esp_err_t err = httpd_start(&server_, &cfg);
  if (err != ESP_OK) return err;

  auto reg = [&](const char* uri, httpd_method_t method,
                 esp_err_t (*h)(httpd_req_t*)) {
    const httpd_uri_t entry = {.uri = uri,
                               .method = method,
                               .handler = h,
                               .user_ctx = this};
    return httpd_register_uri_handler(server_, &entry);
  };

  // Implemented endpoints.
  reg("/api/status", HTTP_GET, TrampolineStatus);
  reg("/api/system_status", HTTP_GET, TrampolineSystemStatus);
  reg("/api/full_refresh", HTTP_POST, TrampolineFullRefresh);
  reg("/api/identify", HTTP_POST, TrampolineIdentify);
  reg("/api/restart", HTTP_POST, TrampolineRestart);
  reg("/api/show/screen", HTTP_POST, TrampolineShowScreen);
  reg("/api/show/currency", HTTP_POST, TrampolineShowCurrency);
  reg("/api/screen/next", HTTP_POST, TrampolineScreenNext);
  reg("/api/screen/previous", HTTP_POST, TrampolineScreenPrev);
  reg("/api/stop_datasources", HTTP_POST, TrampolineStopDataSources);
  reg("/api/restart_datasources", HTTP_POST, TrampolineRestartDataSources);
  reg("/api/frontlight/on", HTTP_POST, TrampolineFrontlightOn);
  reg("/api/frontlight/off", HTTP_POST, TrampolineFrontlightOff);
  reg("/api/frontlight/flash", HTTP_POST, TrampolineFrontlightFlash);
  reg("/api/frontlight/status", HTTP_GET, TrampolineFrontlightStatus);
  reg("/api/frontlight/brightness", HTTP_POST, TrampolineFrontlightBrightness);
  reg("/api/wifi_set_tx_power", HTTP_POST, TrampolineWifiTxPower);

  // CORS preflights. Browsers send OPTIONS before any non-simple
  // cross-origin request (Content-Type: application/json qualifies).
  reg("/api/*", HTTP_OPTIONS, TrampolineOptions);

  // Stubs — see swagger.yml; these need their subsystems ported first.
  // Track via the named beads issues or "pending" when none exists.
  struct Stub {
    const char* uri;
    httpd_method_t method;
    const char* tracking;
  };
  static const Stub kStubs[] = {
      // Full settings CRUD — blocked on the settings-port issue.
      {"/api/settings", HTTP_GET, "pending"},
      {"/api/settings", HTTP_PATCH, "pending"},
      // DND subsystem — no LED handler yet.
      {"/api/dnd/status", HTTP_GET, "pending"},
      {"/api/dnd/enable", HTTP_POST, "pending"},
      {"/api/dnd/disable", HTTP_POST, "pending"},
      // Lights — no WS2812B write API on LedController yet.
      {"/api/lights", HTTP_GET, "pending"},
      {"/api/lights/set", HTTP_POST, "pending"},
      {"/api/lights/color", HTTP_POST, "pending"},
      {"/api/lights/off", HTTP_POST, "pending"},
      // Pause/resume timer — needs a timer_active flag the event loop
      // honours. Not plumbed in the PoC.
      {"/api/action/pause", HTTP_POST, "pending"},
      {"/api/action/timer_restart", HTTP_POST, "pending"},
      // Misc.
      {"/api/show/text", HTTP_POST, "pending"},
      {"/api/show/custom", HTTP_POST, "pending"},
      // OTA — not in the PoC scope (tracked by btclock_v3_fci-5b2).
      {"/api/firmware/auto_update", HTTP_POST, "btclock_v3_fci-5b2"},
      {"/upload/firmware", HTTP_POST, "btclock_v3_fci-5b2"},
      {"/upload/webui", HTTP_POST, "btclock_v3_fci-5b2"},
  };
  for (const auto& s : kStubs) {
    const httpd_uri_t entry = {.uri = s.uri,
                               .method = s.method,
                               .handler = TrampolineNotImplemented,
                               // For stubs the handler needs to know
                               // *which* tracking token to emit, so
                               // user_ctx points at the token itself
                               // rather than `this`.
                               .user_ctx = const_cast<char*>(s.tracking)};
    httpd_register_uri_handler(server_, &entry);
  }

  ESP_LOGI(kTag, "control API listening on http://%s/",
           cfg_.wifi ? cfg_.wifi->ip().c_str() : "?");
  return ESP_OK;
}

// --- Handler trampolines -------------------------------------------
// Each trampoline pulls `this` off user_ctx and dispatches. user_ctx
// points to the ControlServer for live handlers and to a stub-tracking
// C-string for 501 handlers.

esp_err_t ControlServer::TrampolineStatus(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleStatus(req);
}
esp_err_t ControlServer::TrampolineSystemStatus(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleSystemStatus(req);
}
esp_err_t ControlServer::TrampolineFullRefresh(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleFullRefresh(req);
}
esp_err_t ControlServer::TrampolineIdentify(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleIdentify(req);
}
esp_err_t ControlServer::TrampolineRestart(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleRestart(req);
}
esp_err_t ControlServer::TrampolineShowScreen(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleShowScreen(req);
}
esp_err_t ControlServer::TrampolineShowCurrency(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleShowCurrency(req);
}
esp_err_t ControlServer::TrampolineScreenNext(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleScreenNext(req);
}
esp_err_t ControlServer::TrampolineScreenPrev(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleScreenPrev(req);
}
esp_err_t ControlServer::TrampolineStopDataSources(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleStopDataSources(req);
}
esp_err_t ControlServer::TrampolineRestartDataSources(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleRestartDataSources(req);
}
esp_err_t ControlServer::TrampolineFrontlightOn(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleFrontlightOn(req);
}
esp_err_t ControlServer::TrampolineFrontlightOff(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleFrontlightOff(req);
}
esp_err_t ControlServer::TrampolineFrontlightFlash(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleFrontlightFlash(req);
}
esp_err_t ControlServer::TrampolineFrontlightStatus(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleFrontlightStatus(req);
}
esp_err_t ControlServer::TrampolineFrontlightBrightness(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleFrontlightBrightness(req);
}
esp_err_t ControlServer::TrampolineWifiTxPower(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleWifiTxPower(req);
}

esp_err_t ControlServer::TrampolineNotImplemented(httpd_req_t* req) {
  // user_ctx is the tracking-token C-string (not a ControlServer*).
  const char* tracking =
      req->user_ctx ? static_cast<const char*>(req->user_ctx) : "pending";
  return SendNotImplemented(req, tracking);
}

// --- Implemented handlers ------------------------------------------

esp_err_t ControlServer::HandleStatus(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }

  LiveStatus live;
  {
    std::lock_guard<std::mutex> lk(status_mu_);
    live = status_;
  }

  cJSON_AddNumberToObject(root, "currentScreen", live.current_slot);
  cJSON_AddNumberToObject(root, "numScreens", live.slot_count);
  cJSON_AddBoolToObject(root, "timerRunning", live.timer_running);
  cJSON_AddBoolToObject(root, "isOTAUpdating", false);  // TODO: OTA port

  const int64_t uptime_s = esp_timer_get_time() / 1000000;
  cJSON_AddNumberToObject(root, "espUptime", static_cast<double>(uptime_s));
  cJSON_AddNumberToObject(
      root, "espFreeHeap",
      static_cast<double>(esp_get_free_heap_size()));
  cJSON_AddNumberToObject(
      root, "espHeapSize",
      static_cast<double>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)));

  cJSON* conn = cJSON_AddObjectToObject(root, "connectionStatus");
  // The PoC currently only runs the btclock WS v2 source. "V2" tracks
  // that; "price" and "blocks" are legacy BTCLOCK_SOURCE shims the
  // WebUI still renders. Emit plausible defaults keyed to V2.
  const bool v2_up = cfg_.hub != nullptr;
  cJSON_AddBoolToObject(conn, "price", v2_up);
  cJSON_AddBoolToObject(conn, "blocks", v2_up);
  cJSON_AddBoolToObject(conn, "V2", v2_up);
  cJSON_AddBoolToObject(conn, "nostr", false);

  cJSON_AddNumberToObject(root, "rssi", CurrentRssi());
  cJSON_AddStringToObject(root, "currency",
                          live.currency.empty() ? "" : live.currency.c_str());

  // `data` — per-panel content. The old firmware mirrored the text
  // currently on each EPD; we don't maintain that mirror yet, so emit
  // empty strings of the right length so the WebUI doesn't crash.
  // TODO(btclock_v3_fci-mmn follow-up): plumb screen text mirror.
  cJSON* data = cJSON_AddArrayToObject(root, "data");
  for (size_t i = 0; i < cfg_.num_screens; ++i) {
    cJSON_AddItemToArray(data, cJSON_CreateString(""));
  }

  // DND — no subsystem yet. Emit off/false placeholders matching the
  // DndNestedStatus schema so the WebUI renders the block untouched.
  // TODO: port DND subsystem.
  cJSON* dnd = cJSON_AddObjectToObject(root, "dnd");
  cJSON_AddBoolToObject(dnd, "enabled", false);
  cJSON_AddBoolToObject(dnd, "dndTimeEnabled", false);
  cJSON_AddStringToObject(dnd, "startTime", "00:00");
  cJSON_AddStringToObject(dnd, "endTime", "00:00");
  cJSON_AddBoolToObject(dnd, "active", false);

  // LEDs — no readback API yet. Empty array matches ArrayOfLeds shape.
  cJSON_AddItemToObject(root, "leds", cJSON_CreateArray());

  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleSystemStatus(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }

  cJSON_AddNumberToObject(
      root, "espFreeHeap",
      static_cast<double>(esp_get_free_heap_size()));
  cJSON_AddNumberToObject(
      root, "espHeapSize",
      static_cast<double>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)));

  const size_t psram_free =
      esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                                 : 0;
  cJSON_AddNumberToObject(root, "espFreePsram",
                          static_cast<double>(psram_free));
  cJSON_AddNumberToObject(root, "espPsramSize",
                          static_cast<double>(esp_psram_get_size()));

  // LittleFS usage — zero-out on error so the WebUI panel still
  // renders. Mount lives in main.cpp (see beads-bq0 / btclock_fs).
  size_t fs_used = 0;
  size_t fs_total = 0;
  (void)btclock::GetLittleFsUsage(&fs_used, &fs_total);
  cJSON_AddNumberToObject(root, "fsUsedBytes", static_cast<double>(fs_used));
  cJSON_AddNumberToObject(root, "fsTotalBytes", static_cast<double>(fs_total));

  cJSON_AddNumberToObject(root, "rssi", CurrentRssi());
  int8_t tx = 0;
  esp_wifi_get_max_tx_power(&tx);
  cJSON_AddNumberToObject(root, "txPower", tx);

  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleFullRefresh(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kFullRefresh};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleIdentify(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kIdentify};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleRestart(httpd_req_t* req) {
  // Old firmware flushes the response body before calling esp_restart
  // via a delayed task — the connection has to close cleanly or the
  // WebUI gets a TCP-reset error. Do the same: respond first, post
  // kRestart command, main will run esp_restart after a short delay.
  SendEmptyOk(req);
  ControlCommand cmd{ControlCommand::Kind::kRestart};
  PostCommand(cmd);
  return ESP_OK;
}

esp_err_t ControlServer::HandleShowScreen(httpd_req_t* req) {
  char buf[16];
  if (!QueryParam(req, "s", buf, sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing s");
    return ESP_FAIL;
  }
  const int idx = atoi(buf);
  ControlCommand cmd{ControlCommand::Kind::kShowScreen};
  cmd.arg_i = idx;
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleShowCurrency(httpd_req_t* req) {
  char buf[16];
  if (!QueryParam(req, "c", buf, sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing c");
    return ESP_FAIL;
  }
  // Match against active currencies; 404 if not present matches the
  // old firmware's isActiveCurrency() check.
  std::string req_ccy(buf);
  std::transform(req_ccy.begin(), req_ccy.end(), req_ccy.begin(), ::toupper);
  const bool active =
      std::any_of(cfg_.currencies.begin(), cfg_.currencies.end(),
                  [&](const std::string& c) { return c == req_ccy; });
  if (!active) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "currency not active");
    return ESP_FAIL;
  }
  ControlCommand cmd{ControlCommand::Kind::kShowCurrency};
  std::snprintf(cmd.arg_s, sizeof(cmd.arg_s), "%s", req_ccy.c_str());
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleScreenNext(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kNextScreen};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleScreenPrev(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kPrevScreen};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleStopDataSources(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kStopDataSources};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleRestartDataSources(httpd_req_t* req) {
  ControlCommand cmd{ControlCommand::Kind::kRestartDataSources};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

// --- Frontlight handlers ------------------------------------------
// On boards without a PCA9685 backlight (Rev A, V8) `cfg_.frontlight`
// is null; in that case all five endpoints respond 503 so the WebUI
// can surface "frontlight not present on this board" rather than a
// misleading 501.

esp_err_t ControlServer::HandleFrontlightOn(httpd_req_t* req) {
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] =
        "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->On();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightOff(httpd_req_t* req) {
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] =
        "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->Off();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightFlash(httpd_req_t* req) {
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] =
        "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->Flash();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightStatus(httpd_req_t* req) {
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] =
        "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  // Match the old firmware's shape: `{"flStatus":[<per-channel-duty>…]}`.
  // The controller writes the same duty to every channel, so we echo
  // the current duty `num_screens` times — enough to keep the WebUI's
  // fixed-length array happy.
  const FrontlightIface::Status st = cfg_.frontlight->GetStatus();
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  cJSON* arr = cJSON_AddArrayToObject(root, "flStatus");
  const size_t count = cfg_.num_screens > 0 ? cfg_.num_screens : 1;
  for (size_t i = 0; i < count; ++i) {
    cJSON_AddItemToArray(arr,
                         cJSON_CreateNumber(static_cast<double>(st.current_duty)));
  }
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleFrontlightBrightness(httpd_req_t* req) {
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] =
        "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  // Old firmware accepts `?b=<value>` on the query string (and a path
  // variant that rewrites onto the query). Keep the query form — the
  // existing WebUI already calls it that way.
  char buf[16];
  if (!QueryParam(req, "b", buf, sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing b");
    return ESP_FAIL;
  }
  const int raw = atoi(buf);
  if (raw < 0 || raw > 65535) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "out of range");
    return ESP_FAIL;
  }
  cfg_.frontlight->SetBrightness(static_cast<uint16_t>(raw));
  return SendEmptyOk(req);
}

// --- WiFi TX-power ------------------------------------------------
// Body is JSON `{"txPower": <quarter-dBm int>}`. Matches the units of
// esp_wifi_set_max_tx_power (int8_t quarter-dBm). Accepts a modest
// request size — the body is a single field.

esp_err_t ControlServer::HandleWifiTxPower(httpd_req_t* req) {
  constexpr size_t kMaxBody = 128;
  if (req->content_len == 0 || req->content_len > kMaxBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  char body[kMaxBody + 1];
  int total = 0;
  while (total < static_cast<int>(req->content_len)) {
    const int r = httpd_req_recv(req, body + total,
                                 static_cast<size_t>(
                                     static_cast<int>(req->content_len) - total));
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
      return ESP_FAIL;
    }
    total += r;
  }
  body[total] = '\0';
  cJSON* root = cJSON_Parse(body);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  const cJSON* tx = cJSON_GetObjectItemCaseSensitive(root, "txPower");
  if (!cJSON_IsNumber(tx)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "txPower missing");
    return ESP_FAIL;
  }
  const int raw = static_cast<int>(tx->valuedouble);
  cJSON_Delete(root);
  if (!IsValidWifiTxPower(raw)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "out of range");
    return ESP_FAIL;
  }
  if (esp_wifi_set_max_tx_power(static_cast<int8_t>(raw)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "esp_wifi_set_max_tx_power");
    return ESP_FAIL;
  }
  return SendJson(req, "{\"setTxPower\":\"ok\"}");
}

}  // namespace btclock
