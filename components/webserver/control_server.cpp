#include "control_server.hpp"
#include "control_validators.hpp"
#include "mime.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include "cJSON.h"
#include "settings/api.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
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

// --- Static WebUI helpers ------------------------------------------
// All static assets live under <LittleFS base>/www/. Documented here
// rather than next to the MountLittleFs call so the path assumption is
// visible at the use site.
constexpr const char* kWebRootBase = "/lfs/www";

// Strip a query string, decode a minimal %-escape set, and guard
// against path traversal. Returns false if the result would escape the
// web root (`..` segment, NUL byte, absolute path escape).
bool NormaliseUriPath(std::string_view uri, std::string* out_rel) {
  out_rel->clear();
  // Drop query + fragment. We don't consume them — the control-server
  // helpers already handle query params directly via esp_http_server.
  const size_t q = uri.find_first_of("?#");
  if (q != std::string_view::npos) uri.remove_suffix(uri.size() - q);

  if (uri.empty()) return false;
  if (uri.front() != '/') return false;  // defensive

  // Decode %xx in-place into a scratch string. Reject NULs.
  std::string decoded;
  decoded.reserve(uri.size());
  for (size_t i = 0; i < uri.size(); ++i) {
    const char c = uri[i];
    if (c == '%' && i + 2 < uri.size()) {
      auto hex = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
      };
      const int hi = hex(uri[i + 1]);
      const int lo = hex(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        const char byte = static_cast<char>((hi << 4) | lo);
        if (byte == '\0') return false;
        decoded.push_back(byte);
        i += 2;
        continue;
      }
    }
    if (c == '\0') return false;
    decoded.push_back(c);
  }

  // Split on '/', reject '..' and '.' segments, normalise doubled
  // slashes. We build a clean relative path (no leading slash) so the
  // caller can concatenate kWebRootBase safely.
  std::string out;
  out.reserve(decoded.size());
  size_t i = 1;  // skip leading '/'
  while (i <= decoded.size()) {
    size_t j = decoded.find('/', i);
    if (j == std::string::npos) j = decoded.size();
    const std::string_view seg(decoded.data() + i, j - i);
    if (seg == ".." || seg.find('\0') != std::string_view::npos) return false;
    if (!seg.empty() && seg != ".") {
      if (!out.empty()) out.push_back('/');
      out.append(seg.data(), seg.size());
    }
    i = j + 1;
  }

  // A trailing slash in the request (or an empty URI after stripping
  // the leading '/') resolves to "<dir>/index.html". Mirror the old
  // AsyncStaticWebHandler::setDefaultFile("index.html") behaviour.
  const bool trailing_slash = !decoded.empty() && decoded.back() == '/';
  if (out.empty() || trailing_slash) {
    if (!out.empty()) out.push_back('/');
    out.append("index.html");
  }

  *out_rel = std::move(out);
  return true;
}

bool RequestAcceptsGzip(httpd_req_t* req) {
  // The old firmware assumes every browser accepts gzip (AsyncStatic
  // encodes unconditionally). We still honour the header if the client
  // explicitly omits "gzip" — curl --compressed off, test clients,
  // etc. — to avoid surprising a caller that genuinely can't decode.
  char buf[96];
  if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof(buf)) !=
      ESP_OK) {
    return false;
  }
  std::string_view v(buf);
  // Simple substring check — good enough for the handful of
  // User-Agents a captive WebUI will see. Full parsing with q-values
  // is overkill.
  return v.find("gzip") != std::string_view::npos;
}

bool FileExists(const std::string& abs_path) {
  struct stat st;
  return stat(abs_path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Format the per-pixel array under the key "data" — this is what the
// WebUI (and SSE stream) consume. Pixel 0 is the *last* one in the
// array, matching the old firmware's numPixels()-i-1 ordering (the
// strip is physically daisy-chained with index 0 nearest the solder
// pad; the UI numbers them from the leftmost panel).
cJSON* BuildLightsStatusArray(const LedsIface::Status& st) {
  cJSON* arr = cJSON_CreateArray();
  if (!arr) return nullptr;
  for (uint32_t i = 0; i < st.pixel_count; ++i) {
    const uint32_t px = st.pixels[st.pixel_count - 1 - i];
    const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>(px & 0xFFu);
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "red", r);
    cJSON_AddNumberToObject(obj, "green", g);
    cJSON_AddNumberToObject(obj, "blue", b);
    cJSON_AddStringToObject(obj, "hex", hex);
    cJSON_AddItemToArray(arr, obj);
  }
  return arr;
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
  reg("/upload/webui", HTTP_POST, TrampolineUploadWebui);
  reg("/api/lights", HTTP_GET, TrampolineLightsStatus);
  reg("/api/lights/color", HTTP_POST, TrampolineLightsColor);
  reg("/api/lights/off", HTTP_POST, TrampolineLightsOff);
  reg("/api/lights/set", HTTP_POST, TrampolineLightsSet);
  reg("/api/settings", HTTP_GET, TrampolineSettingsGet);
  reg("/api/settings", HTTP_PATCH, TrampolineSettingsPatch);
  reg("/api/dnd/status", HTTP_GET, TrampolineDndStatus);
  reg("/api/dnd/enable", HTTP_POST, TrampolineDndEnable);
  reg("/api/dnd/disable", HTTP_POST, TrampolineDndDisable);
  reg("/api/action/pause", HTTP_POST, TrampolineActionPause);
  reg("/api/action/timer_restart", HTTP_POST, TrampolineActionTimerRestart);
  // TODO(btclock_v3_fci-equ): `GET /` static-file serve from LittleFS.
  // Tracked separately; the upload path above lands bytes on flash,
  // but until the static server lands the WebUI has no way to serve
  // those bytes to a browser.

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
      // Misc.
      {"/api/show/text", HTTP_POST, "pending"},
      {"/api/show/custom", HTTP_POST, "pending"},
      // OTA — not in the PoC scope (tracked by btclock_v3_fci-5b2).
      {"/api/firmware/auto_update", HTTP_POST, "btclock_v3_fci-5b2"},
      {"/upload/firmware", HTTP_POST, "btclock_v3_fci-5b2"},
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

  // Static WebUI catch-all. MUST be registered last: esp_http_server
  // walks hd_calls in registration order and returns the first match
  // (see esp-idf components/esp_http_server/src/httpd_uri.c:
  // `httpd_find_uri_handler` loops forward; the first hit wins). A
  // wildcard /* registered earlier would also cause every subsequent
  // `httpd_register_uri_handler` call for the same method to fail with
  // ESP_ERR_HTTPD_HANDLER_EXISTS, because registration itself checks
  // whether any existing handler already matches the new URI.
  //
  // Only GET is wildcarded here; the above `/api/*` OPTIONS handler
  // covers browser CORS preflight, and POST/PATCH routes all live
  // under /api/ — a static-file POST would be nonsense anyway.
  reg("/*", HTTP_GET, TrampolineStatic);

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
esp_err_t ControlServer::TrampolineUploadWebui(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleUploadWebui(req);
}
esp_err_t ControlServer::TrampolineLightsStatus(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleLightsStatus(req);
}
esp_err_t ControlServer::TrampolineLightsColor(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleLightsColor(req);
}
esp_err_t ControlServer::TrampolineLightsOff(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleLightsOff(req);
}
esp_err_t ControlServer::TrampolineLightsSet(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleLightsSet(req);
}
esp_err_t ControlServer::TrampolineSettingsGet(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleSettingsGet(req);
}
esp_err_t ControlServer::TrampolineSettingsPatch(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleSettingsPatch(req);
}
esp_err_t ControlServer::TrampolineDndStatus(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleDndStatus(req);
}
esp_err_t ControlServer::TrampolineDndEnable(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleDndEnable(req);
}
esp_err_t ControlServer::TrampolineDndDisable(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleDndDisable(req);
}
esp_err_t ControlServer::TrampolineActionPause(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleActionPause(req);
}
esp_err_t ControlServer::TrampolineActionTimerRestart(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleActionTimerRestart(req);
}

esp_err_t ControlServer::TrampolineStatic(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleStatic(req);
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
  // `timerRunning` mirrors the old firmware's isTimerActive() — false
  // while the screen-rotation pause is armed. Prefer the real timer
  // iface when plumbed so this stays accurate even if the main loop
  // hasn't had a chance to re-publish LiveStatus yet.
  const bool timer_running =
      cfg_.timer ? !cfg_.timer->IsPaused() : live.timer_running;
  cJSON_AddBoolToObject(root, "timerRunning", timer_running);
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

  // `data` — per-panel content. main.cpp refreshes `panel_texts` on
  // every render via ScreenManager::last_panel_texts(); we pad with
  // empty strings when the mirror hasn't caught up (e.g. first boot
  // before the first successful render).
  cJSON* data = cJSON_AddArrayToObject(root, "data");
  for (size_t i = 0; i < cfg_.num_screens; ++i) {
    const char* s = (i < live.panel_texts.size())
                        ? live.panel_texts[i].c_str()
                        : "";
    cJSON_AddItemToArray(data, cJSON_CreateString(s));
  }

  // DND nested status. Shape pinned to the old firmware's
  // src/lib/net/webserver/status.cpp so the WebUI's DndNestedStatus
  // contract is preserved. Falls back to the inactive stub when the
  // DND subsystem wasn't wired (should only happen if cfg_.dnd is
  // null, e.g. in a host-simulated server).
  cJSON* dnd = cJSON_AddObjectToObject(root, "dnd");
  DndIface::Status ds{};
  if (cfg_.dnd) ds = cfg_.dnd->GetStatus();
  cJSON_AddBoolToObject(dnd, "enabled", ds.enabled);
  cJSON_AddBoolToObject(dnd, "dndTimeEnabled", ds.time_enabled);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%u:%02u",
                static_cast<unsigned>(ds.start_hour),
                static_cast<unsigned>(ds.start_minute));
  cJSON_AddStringToObject(dnd, "startTime", buf);
  std::snprintf(buf, sizeof(buf), "%u:%02u",
                static_cast<unsigned>(ds.end_hour),
                static_cast<unsigned>(ds.end_minute));
  cJSON_AddStringToObject(dnd, "endTime", buf);
  cJSON_AddBoolToObject(dnd, "active", ds.active);

  // LEDs — mirror the per-pixel state, same shape /api/lights/status
  // returns. BuildLightsStatusArray reverses the index order to match
  // the old firmware's numPixels-i-1 convention.
  if (cfg_.leds) {
    const LedsIface::Status st = cfg_.leds->GetStatus();
    cJSON* leds = BuildLightsStatusArray(st);
    if (leds) cJSON_AddItemToObject(root, "leds", leds);
    else cJSON_AddItemToObject(root, "leds", cJSON_CreateArray());
  } else {
    cJSON_AddItemToObject(root, "leds", cJSON_CreateArray());
  }

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

// --- Lights handlers ---------------------------------------------
// The /api/lights/* surface mirrors the old Arduino firmware's
// src/lib/net/webserver/lights.cpp shapes 1:1 so the existing WebUI
// drives this port unchanged:
//
//   GET  /api/lights          -> [{"red":R,"green":G,"blue":B,"hex":"#RRGGBB"}, ...]
//   POST /api/lights/color?c=RRGGBB  (or "off") -> same status body
//   POST /api/lights/off      -> 200 OK, empty body
//   POST /api/lights/set      -> body is a JSON array of per-pixel
//                                objects {"red":..,"green":..,"blue":..}
//                                or {"hex":"#RRGGBB"}.
//
// TODO(auth): gate behind HTTP Basic auth once the auth subsystem
// lands (same deferred TODO as the rest of the control server).

namespace {

// Common 503 when the LED subsystem isn't present. Every shipping
// BTClock has a NeoPixel strip, so this is effectively an "LED init
// failed" path — not a board-variant gate.
esp_err_t SendLedsUnavailable(httpd_req_t* req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char kBody[] = "{\"error\":\"leds not available\"}";
  return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse `RRGGBB` or `#RRGGBB`. Returns true on success. Does not accept
// the 3-digit shorthand — old firmware's sscanf("%2x%2x%2x") didn't.
bool ParseHex6(std::string_view s, uint32_t* out) {
  if (!s.empty() && s.front() == '#') s.remove_prefix(1);
  if (s.size() != 6) return false;
  uint32_t acc = 0;
  for (size_t i = 0; i < 6; ++i) {
    const int d = HexDigit(s[i]);
    if (d < 0) return false;
    acc = (acc << 4) | static_cast<uint32_t>(d);
  }
  *out = acc & 0x00FFFFFFu;
  return true;
}

// Read the full request body into a malloc'd, NUL-terminated buffer.
// Caller owns + frees. Returns nullptr on error.
char* ReadFullBody(httpd_req_t* req, size_t max_bytes) {
  if (req->content_len == 0 || req->content_len > max_bytes) return nullptr;
  char* buf = static_cast<char*>(malloc(req->content_len + 1));
  if (!buf) return nullptr;
  int total = 0;
  while (total < static_cast<int>(req->content_len)) {
    const int r =
        httpd_req_recv(req, buf + total,
                       static_cast<size_t>(
                           static_cast<int>(req->content_len) - total));
    if (r <= 0) {
      free(buf);
      return nullptr;
    }
    total += r;
  }
  buf[total] = '\0';
  return buf;
}

}  // namespace

esp_err_t ControlServer::HandleLightsStatus(httpd_req_t* req) {
  if (!cfg_.leds) return SendLedsUnavailable(req);
  const LedsIface::Status st = cfg_.leds->GetStatus();
  cJSON* arr = BuildLightsStatusArray(st);
  if (!arr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  char* txt = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleLightsColor(httpd_req_t* req) {
  if (!cfg_.leds) return SendLedsUnavailable(req);
  char buf[16];
  if (!QueryParam(req, "c", buf, sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing c");
    return ESP_FAIL;
  }
  uint32_t rgb = 0;
  if (std::string_view(buf) == "off") {
    rgb = 0;
  } else if (!ParseHex6(buf, &rgb)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad color");
    return ESP_FAIL;
  }
  cfg_.leds->SetSolidColor(rgb);
  // Echo the post-change state, matching the old firmware's
  // onApiLightsSetColor which serialises buildLedStatusJson()["data"].
  const LedsIface::Status st = cfg_.leds->GetStatus();
  cJSON* arr = BuildLightsStatusArray(st);
  if (!arr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  char* txt = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleLightsOff(httpd_req_t* req) {
  if (!cfg_.leds) return SendLedsUnavailable(req);
  cfg_.leds->SetSolidColor(0);
  // Old firmware returns 200 OK with empty body.
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleLightsSet(httpd_req_t* req) {
  if (!cfg_.leds) return SendLedsUnavailable(req);
  constexpr size_t kMaxBody = 1024;
  char* body = ReadFullBody(req, kMaxBody);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  cJSON* root = cJSON_Parse(body);
  free(body);
  if (!root || !cJSON_IsArray(root)) {
    if (root) cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  const LedsIface::Status st_before = cfg_.leds->GetStatus();
  const int n = cJSON_GetArraySize(root);
  if (n == 0) {
    cJSON_Delete(root);
    cfg_.leds->SetSolidColor(0);
    return SendEmptyOk(req);
  }
  if (n != static_cast<int>(st_before.pixel_count)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pixel count mismatch");
    return ESP_FAIL;
  }
  uint32_t pixels[8] = {0};
  for (int i = 0; i < n && i < static_cast<int>(sizeof(pixels) /
                                                sizeof(pixels[0])); ++i) {
    cJSON* entry = cJSON_GetArrayItem(root, i);
    if (!entry || !cJSON_IsObject(entry)) {
      cJSON_Delete(root);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad entry");
      return ESP_FAIL;
    }
    const cJSON* hex = cJSON_GetObjectItemCaseSensitive(entry, "hex");
    const cJSON* r = cJSON_GetObjectItemCaseSensitive(entry, "red");
    const cJSON* g = cJSON_GetObjectItemCaseSensitive(entry, "green");
    const cJSON* b = cJSON_GetObjectItemCaseSensitive(entry, "blue");
    uint32_t rgb = 0;
    if (cJSON_IsString(hex) && hex->valuestring) {
      if (!ParseHex6(hex->valuestring, &rgb)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad hex");
        return ESP_FAIL;
      }
    } else if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
      const uint32_t rv = static_cast<uint32_t>(r->valuedouble) & 0xFFu;
      const uint32_t gv = static_cast<uint32_t>(g->valuedouble) & 0xFFu;
      const uint32_t bv = static_cast<uint32_t>(b->valuedouble) & 0xFFu;
      rgb = (rv << 16) | (gv << 8) | bv;
    } else {
      cJSON_Delete(root);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
      return ESP_FAIL;
    }
    // Old firmware stores with reversed indexing (pixel i comes from
    // JSON[n-1-i]); mirror that so the WebUI's left-to-right order is
    // preserved.
    pixels[n - 1 - i] = rgb;
  }
  cJSON_Delete(root);
  cfg_.leds->SetPixels(pixels, static_cast<uint32_t>(n));
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

// --- /upload/webui ------------------------------------------------
// Streams a LittleFS image into the `storage` partition, then reboots
// so the new filesystem is mounted clean. Mirrors the old firmware's
// U_SPIFFS flow (src/lib/net/webserver/ota_routes.cpp asyncFileUpdateHandler
// command == U_SPIFFS).
//
// TODO(auth): require HTTP Basic auth once btclock_v3_fci-equ's auth
// gate lands. A hostile network can brick the device today — anyone
// can POST an arbitrary blob and force a reboot. DO NOT expose this
// endpoint to the public internet until the gate is in.

namespace {

// Trampoline for FlashWebuiImage — it expects a C-style callback.
// `ctx` is the httpd_req_t*; read up to `want` bytes into `buf`.
int HttpdRecvTrampoline(void* ctx, char* buf, size_t want) {
  auto* req = static_cast<httpd_req_t*>(ctx);
  const int r = httpd_req_recv(req, buf, want);
  // HTTPD_SOCK_ERR_TIMEOUT can happen on slow uploads; the old firmware
  // treated these as fatal for OTA flows (the client is expected to
  // push at line rate) — mirror that. Any <=0 result aborts the stream.
  return r;
}

// Deferred reboot callback. Scheduled with a short delay so the HTTP
// response has flushed to the client's socket before the reboot fires;
// otherwise the client sees a TCP-reset and surfaces a generic error
// instead of the "reboot scheduled" body we sent.
void RebootTimerCallback(void* /*arg*/) { esp_restart(); }

void ScheduleReboot(uint32_t delay_ms) {
  esp_timer_create_args_t targs = {};
  targs.callback = &RebootTimerCallback;
  targs.name = "webui_reboot";
  esp_timer_handle_t t = nullptr;
  if (esp_timer_create(&targs, &t) != ESP_OK) {
    // Fallback: if the timer can't be created, just reboot inline —
    // the response may be truncated but the device recovers.
    ESP_LOGW(kTag, "reboot timer create failed; rebooting inline");
    esp_restart();
    return;
  }
  esp_timer_start_once(t, static_cast<uint64_t>(delay_ms) * 1000);
}

}  // namespace

esp_err_t ControlServer::HandleUploadWebui(httpd_req_t* req) {
  const size_t part_size = btclock::GetLittleFsPartitionSize();
  if (part_size == 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "storage partition not found");
    return ESP_FAIL;
  }

  // Content-Length gate. `req->content_len` is 0 when the header is
  // absent; esp_http_server populates it from the header otherwise.
  // Reject oversize with 413 up-front so we don't erase the partition
  // just to then abort mid-stream.
  const size_t expected = req->content_len;
  if (expected > part_size) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char body[96];
    std::snprintf(body, sizeof(body),
                  "{\"error\":\"oversize\",\"max\":%u}",
                  static_cast<unsigned>(part_size));
    httpd_resp_send(req, body, strlen(body));
    return ESP_FAIL;
  }

  ESP_LOGW(kTag, "webui upload starting: content-length=%u partition=%u",
           static_cast<unsigned>(expected),
           static_cast<unsigned>(part_size));

  size_t written = 0;
  const esp_err_t rc = btclock::FlashWebuiImage(
      &HttpdRecvTrampoline, req, expected, &written);

  if (rc == ESP_ERR_INVALID_SIZE) {
    // Truncated upload: the declared Content-Length wasn't delivered.
    // Partition is in an undefined state, but MountLittleFs's
    // format_if_mount_failed=true will re-format on the next boot if
    // the partial image fails to mount. Still respond 500 so the
    // client retries rather than assumes success.
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "truncated upload");
    return ESP_FAIL;
  }
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "webui upload failed: %s", esp_err_to_name(rc));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(rc));
    return ESP_FAIL;
  }

  // Respond first, then reboot. The 500 ms delay in ScheduleReboot
  // gives esp_http_server time to hand the response bytes to lwIP and
  // for the client's TCP stack to ACK before the device vanishes.
  ESP_LOGW(kTag, "webui upload ok: bytes=%u; rebooting in 500ms",
           static_cast<unsigned>(written));
  char body[96];
  std::snprintf(body, sizeof(body),
                "{\"result\":\"ok\",\"bytes\":%u}",
                static_cast<unsigned>(written));
  SendJson(req, body);
  ScheduleReboot(500);
  return ESP_OK;
}

// --- Static WebUI handler -----------------------------------------
// Serves gzipped and plain assets out of /lfs/www/. Registered as a
// catch-all /* GET handler, so it's the final fallback after every
// explicit /api/* route. Streams the body in small chunks to stay
// within Rev A's internal-DRAM budget.
//
// Auth: deliberately unguarded. The WebUI needs its own bundle (HTML,
// JS, CSS) before the user can possibly authenticate, and the static
// assets don't reveal any state beyond their shipped bytes.
// TODO(auth): once equ's basic-auth gate lands, require it here for
// non-root paths — not for "/" so the browser can load the HTML and
// be prompted for credentials via a 401 on the first /api call.

esp_err_t ControlServer::HandleStatic(httpd_req_t* req) {
  // Throttled 503 log so boot races (LittleFS still mounting) or
  // upload-in-progress conditions don't spam. Per-handler static — we
  // only serve from one thread at a time on the webserver task.
  static int64_t s_last_503_log_us = 0;

  std::string rel;
  if (!NormaliseUriPath(req->uri, &rel)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    const char kMsg[] = "bad path";
    return httpd_resp_send(req, kMsg, sizeof(kMsg) - 1);
  }

  // Fail-closed if the filesystem isn't mounted. Two places this can
  // happen: first-boot before MountLittleFs completes, or a future
  // upload flow that briefly unmounts. Either way the correct answer
  // is 503 + Retry-After, not a misleading 404.
  size_t fs_used = 0, fs_total = 0;
  if (btclock::GetLittleFsUsage(&fs_used, &fs_total) != ESP_OK) {
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_503_log_us > 60LL * 1000 * 1000) {
      ESP_LOGW(kTag, "static asset requested but LittleFS not mounted");
      s_last_503_log_us = now_us;
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Retry-After", "10");
    const char kMsg[] = "filesystem not ready";
    return httpd_resp_send(req, kMsg, sizeof(kMsg) - 1);
  }

  // Build candidate absolute paths. The old firmware *always* prefers
  // the gzipped variant (AsyncStatic re-checks on every request too),
  // but we honour Accept-Encoding: browsers that explicitly exclude
  // gzip still get an uncompressed copy if one exists.
  const std::string base = std::string(kWebRootBase) + "/" + rel;
  const std::string gz = base + ".gz";
  const bool want_gz = RequestAcceptsGzip(req);

  std::string chosen;
  bool chosen_gz = false;
  if (want_gz && FileExists(gz)) {
    chosen = gz;
    chosen_gz = true;
  } else if (FileExists(base)) {
    chosen = base;
  } else if (FileExists(gz)) {
    // Fall back to gzipped even without explicit Accept-Encoding —
    // every modern browser handles it and the old firmware does the
    // same (it doesn't inspect Accept-Encoding at all). This keeps us
    // from returning 404 for gzip-only bundles.
    chosen = gz;
    chosen_gz = true;
  } else {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    const char kMsg[] = "not found";
    return httpd_resp_send(req, kMsg, sizeof(kMsg) - 1);
  }

  FILE* f = std::fopen(chosen.c_str(), "rb");
  if (!f) {
    // Race with unlink, or ENOMEM inside esp_littlefs — either way
    // pretend it's a 404. Logging helps diagnose.
    ESP_LOGW(kTag, "fopen('%s') failed: %d", chosen.c_str(), errno);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    const char kMsg[] = "not found";
    return httpd_resp_send(req, kMsg, sizeof(kMsg) - 1);
  }

  // Headers. Content-Type keys off the *logical* filename (strip .gz).
  const std::string_view mime = MimeTypeForPath(rel);
  httpd_resp_set_type(req, std::string(mime).c_str());
  if (chosen_gz) {
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // Vary: Accept-Encoding so proxies don't hand the .gz to a client
    // that can't decode it. Cheap insurance.
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
  }

  // Caching policy:
  //   * index.html: no-cache. The hashed JS/CSS references inside it
  //     are the only link between a deployed bundle and its assets;
  //     serving a stale index would point the browser at missing
  //     hashes on the next upload. Matches the spirit of the old
  //     firmware (AsyncStatic with no-cache defaults) without
  //     sacrificing asset caching.
  //   * everything else: public, max-age=300 so repeat nav is fast
  //     but an OTA/WebUI upload lands within 5 minutes.
  // Note: the old Arduino server didn't set explicit Cache-Control
  // headers; this is a deliberate improvement on that side.
  const bool is_index_html =
      rel == "index.html" || rel.rfind("/index.html") == rel.size() - 11;
  httpd_resp_set_hdr(req, "Cache-Control",
                     is_index_html ? "no-cache" : "public, max-age=300");

  // Stream the body. 1536 B keeps us well under the 2 KiB stack cushion
  // esp_http_server leaves on the worker task, and is large enough to
  // amortise the per-send TCP overhead on typical 20 KiB JS chunks.
  constexpr size_t kChunk = 1536;
  char buf[kChunk];
  for (;;) {
    const size_t n = std::fread(buf, 1, kChunk, f);
    if (n > 0) {
      if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
        std::fclose(f);
        // Connection likely gone — return ESP_FAIL so esp_http_server
        // doesn't try to follow up with its own response.
        return ESP_FAIL;
      }
    }
    if (n < kChunk) break;  // EOF or short read
  }
  std::fclose(f);
  // Terminator chunk. Required for httpd_resp_send_chunk flow.
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// --- /api/settings GET + PATCH -------------------------------------
// The pure-logic builder/validator lives in components/settings; these
// handlers plumb the esp_http_server request into that core plus a
// btclock::Prefs-backed NVS adapter. Any change to the JSON shape or
// field validation belongs in components/settings/settings_api.cpp —
// this file just handles transport.

namespace {

// Populate a settings::DeviceContext from runtime state. Kept small
// and side-effect free so the GET path doesn't have to touch NVS for
// facts like hwRev or firmware git info.
btclock::settings::DeviceContext BuildDeviceContext(
    const ControlServer::Config& cfg) {
  btclock::settings::DeviceContext ctx;
  // Wifi component doesn't yet surface the configured hostname — the
  // old firmware built it from hostnamePrefix + last-4-MAC. Read those
  // back here so /api/settings hostname echoes whatever the user
  // actually sees on the network.
  {
    btclock::Prefs p("settings");
    const std::string prefix = p.GetString("hostnamePrefix", "btclock");
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[48];
    std::snprintf(host, sizeof(host), "%s-%02x%02x", prefix.c_str(),
                  mac[4], mac[5]);
    ctx.hostname = host;
  }
  ctx.ip = cfg.wifi ? cfg.wifi->ip() : "";
  int8_t tx = 0;
  esp_wifi_get_max_tx_power(&tx);
  ctx.tx_power = tx;
  ctx.num_screens = static_cast<int32_t>(cfg.num_screens);
  ctx.has_frontlight = cfg.frontlight != nullptr;
  ctx.has_light_level = false;  // TODO(bh1750): surface from sensor once wired
  ctx.hw_rev = cfg.hw_name;
  ctx.fs_rev = "";
  const esp_app_desc_t* desc = esp_app_get_description();
  if (desc) {
    ctx.git_rev = desc->version;
    ctx.last_build_time = std::string(desc->date) + " " + desc->time;
  }
  ctx.available_fonts = cfg.available_fonts;
  ctx.available_pools = cfg.available_pools;
  ctx.available_currencies = cfg.available_currencies;
  for (const auto& s : cfg.screens_catalog) {
    ctx.screens.push_back({s.id, s.name});
  }
  return ctx;
}

}  // namespace

esp_err_t ControlServer::HandleSettingsGet(httpd_req_t* req) {
  btclock::settings::NvsPrefs prefs(btclock::prefs::kSettingsNs);
  const auto ctx = BuildDeviceContext(cfg_);
  cJSON* root = btclock::settings::BuildGetResponse(prefs, ctx);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleSettingsPatch(httpd_req_t* req) {
  // 16 KB ceiling — the old firmware's AsyncCallbackJsonWebHandler
  // defaulted to 16384 bytes. Any PATCH larger than this is either
  // the WebUI sending a full object (rare — it PATCHes deltas) or an
  // attacker probing; bounce either way.
  constexpr size_t kMaxBody = 16 * 1024;
  if (req->content_len == 0 || req->content_len > kMaxBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  std::string body;
  body.resize(req->content_len);
  int total = 0;
  while (total < static_cast<int>(req->content_len)) {
    const int r = httpd_req_recv(req, body.data() + total,
                                 static_cast<size_t>(
                                     static_cast<int>(req->content_len) - total));
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
      return ESP_FAIL;
    }
    total += r;
  }

  btclock::settings::NvsPrefs prefs(btclock::prefs::kSettingsNs);
  const auto ctx = BuildDeviceContext(cfg_);
  const auto result =
      btclock::settings::ApplyPatch(body.c_str(), ctx, prefs, prefs);
  if (result.status != btclock::settings::PatchStatus::kOk) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}",
                  result.error.c_str());
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
  }

  // Flush NVS so a quick reboot (if the WebUI posts and then calls
  // /api/restart) doesn't lose the changes.
  prefs.Commit();

  // Re-broadcast runtime-editable fields via the LED controller so
  // /api/lights reflects the new state without a reboot. Only the
  // fields the LED subsystem cares about need to be pushed here —
  // other runtime consumers (DND, screen rotation, timezone) will
  // pick up the new NVS values on their next read.
  // TODO(btclock_v3_fci-equ): wire DND change hooks to LedController.

  // Response body mirrors old firmware: 200 OK with an empty body
  // when no reboot is required, {"rebootRequired":true} otherwise.
  if (result.reboot_required) {
    return SendJson(req, "{\"rebootRequired\":true}");
  }
  return SendEmptyOk(req);
}

// --- DND + timer-pause handlers ------------------------------------

esp_err_t ControlServer::HandleDndStatus(httpd_req_t* req) {
  if (!cfg_.dnd) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no dnd");
    return ESP_FAIL;
  }
  const DndIface::Status ds = cfg_.dnd->GetStatus();
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  cJSON_AddBoolToObject(root, "enabled", ds.enabled);
  cJSON_AddBoolToObject(root, "dndTimeEnabled", ds.time_enabled);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%u:%02u",
                static_cast<unsigned>(ds.start_hour),
                static_cast<unsigned>(ds.start_minute));
  cJSON_AddStringToObject(root, "startTime", buf);
  std::snprintf(buf, sizeof(buf), "%u:%02u",
                static_cast<unsigned>(ds.end_hour),
                static_cast<unsigned>(ds.end_minute));
  cJSON_AddStringToObject(root, "endTime", buf);
  cJSON_AddBoolToObject(root, "active", ds.active);
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleDndEnable(httpd_req_t* req) {
  if (!cfg_.dnd) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no dnd");
    return ESP_FAIL;
  }
  cfg_.dnd->SetEnabled(true);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleDndDisable(httpd_req_t* req) {
  if (!cfg_.dnd) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no dnd");
    return ESP_FAIL;
  }
  cfg_.dnd->SetEnabled(false);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionPause(httpd_req_t* req) {
  if (!cfg_.timer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no timer");
    return ESP_FAIL;
  }
  cfg_.timer->SetPaused(true);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionTimerRestart(httpd_req_t* req) {
  if (!cfg_.timer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no timer");
    return ESP_FAIL;
  }
  // `timer_restart` in the old firmware re-arms the rotation clock,
  // which has the effect of resuming a paused run AND zeroing the
  // deadline so the next rotation is a full period away. Mirror that:
  // unpause first, then reset the deadline.
  cfg_.timer->SetPaused(false);
  cfg_.timer->Restart();
  return SendEmptyOk(req);
}

}  // namespace btclock
