#include "control_server.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>

#include "auth_gate.hpp"
#include "cJSON.h"
#include "control_validators.hpp"
#include "epd/panel.hpp"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "heap_metrics.hpp"
#include "light_metrics.hpp"
#include "littlefs.hpp"
#include "mime.hpp"
#include "net_util/hostname.hpp"
#include "ota_manager.hpp"
#include "ota_upload_bounds.hpp"
#include "pool_logo_fetcher/pool_logo_fetcher.hpp"
#include "queue_metrics.hpp"
#include "settings/api.hpp"
#include "settings/build_time.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "show_text_parse.hpp"
#include "sse_server.hpp"
#include "url_decode.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "ctrl-api";

// Auth — every non-static /api/* handler starts with
// `RequireHttpAuth(req)` which reads `httpAuthEnabled` from NVS and
// either passes through (factory default, or already-authenticated
// credential) or sends a 401 itself. The helper returns false on 401
// and the handler must return ESP_OK so esp_http_server doesn't
// double-respond. HandleStatic and HandleOptions are intentionally
// ungated — see the WHY comments above each.

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

bool QueryParam(httpd_req_t* req, const char* key, char* out, size_t out_size) {
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 256) return false;
  char qbuf[256];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
    return false;
  }
  if (httpd_query_key_value(qbuf, key, out, out_size) != ESP_OK) {
    return false;
  }
  // ESP-IDF returns the raw value verbatim — no percent-decode and no
  // `+` handling. Decode in place so user-typed strings like
  // `?t=%20CLOCK%20` reach the renderer as ` CLOCK ` instead of the
  // literal `%20`s. Malformed escapes turn the whole call into a miss
  // so the caller responds 400 the same way it would for a missing key.
  return btclock::http::UrlDecodeInPlace(out);
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

// Read the full request body into a NUL-terminated buffer. Caller owns
// + must release with heap_caps_free (works for both PSRAM- and
// internal-heap allocations). Returns nullptr on error.
//
// PSRAM-first with internal-heap fallback: /api/show/* bodies can run
// to a few KiB and only live for the duration of the handler, so
// parking them in PSRAM avoids crowding the same DRAM that mbedtls
// fights over during a concurrent handshake. Latency is irrelevant —
// cJSON walks the buffer linearly once.
//
// Hoisted to the top-of-file anonymous namespace so every HandleXxx
// method below can use it — historically only the /api/lights/set
// handler needed it and the helper sat next to that, but the
// /api/show/text + /api/show/custom handlers now share the same
// pattern and live earlier in the file.
char* ReadFullBody(httpd_req_t* req, size_t max_bytes) {
  if (req->content_len == 0 || req->content_len > max_bytes) return nullptr;
  char* buf = static_cast<char*>(heap_caps_malloc_prefer(
      req->content_len + 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
      MALLOC_CAP_8BIT));
  if (!buf) return nullptr;
  int total = 0;
  while (total < static_cast<int>(req->content_len)) {
    const int r = httpd_req_recv(
        req, buf + total,
        static_cast<size_t>(static_cast<int>(req->content_len) - total));
    if (r <= 0) {
      heap_caps_free(buf);
      return nullptr;
    }
    total += r;
  }
  buf[total] = '\0';
  return buf;
}

}  // namespace

// --- ControlServer --------------------------------------------------

ControlServer::ControlServer(Config cfg) : cfg_(std::move(cfg)) {
  status_.current_slot = 0;
  status_.slot_count =
      cfg_.currencies.empty()
          ? 1
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

bool ControlServer::TakePendingCustomCells(std::vector<std::string>* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lk(pending_custom_mu_);
  if (!pending_custom_valid_) return false;
  *out = std::move(pending_custom_cells_);
  pending_custom_cells_.clear();
  pending_custom_valid_ = false;
  return true;
}

void ControlServer::ApplyCors(httpd_req_t* req) {
  // Permissive CORS matches the production firmware, which sets
  // Access-Control-Allow-Origin: * globally (lib/net/webserver/webserver.cpp).
  // Tightening this belongs in the same follow-up that adds HTTP Basic auth.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                     "GET, POST, PATCH, DELETE, OPTIONS");
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
  // Handler cap must cover every reg() in this function + the /* catch-all,
  // with headroom for planned additions (OTA, DND, pause/restart). Overflow
  // silently drops the trailing registrations, including /*, which breaks
  // static serving.
  cfg.max_uri_handlers = 48;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 8192;
  cfg.lru_purge_enable = true;

  esp_err_t err = httpd_start(&server_, &cfg);
  if (err != ESP_OK) return err;

  auto reg = [&](const char* uri, httpd_method_t method,
                 esp_err_t (*h)(httpd_req_t*)) {
    const httpd_uri_t entry = {
        .uri = uri, .method = method, .handler = h, .user_ctx = this};
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
  reg("/api/show/text", HTTP_POST, TrampolineShowText);
  reg("/api/show/custom", HTTP_POST, TrampolineShowCustom);
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
  reg("/api/lights/effect", HTTP_POST, TrampolineLightsEffect);
  reg("/api/settings", HTTP_GET, TrampolineSettingsGet);
  reg("/api/settings", HTTP_PATCH, TrampolineSettingsPatch);
  reg("/api/dnd/status", HTTP_GET, TrampolineDndStatus);
  reg("/api/dnd/enable", HTTP_POST, TrampolineDndEnable);
  reg("/api/dnd/disable", HTTP_POST, TrampolineDndDisable);
  reg("/api/action/pause", HTTP_POST, TrampolineActionPause);
  reg("/api/action/timer_restart", HTTP_POST, TrampolineActionTimerRestart);
  reg("/api/action/simulate_zap", HTTP_POST, TrampolineActionSimulateZap);
  reg("/api/action/clear_pool_logos", HTTP_POST,
      TrampolineActionClearPoolLogos);
  reg("/api/firmware/auto_update", HTTP_POST, TrampolineFirmwareAutoUpdate);
  reg("/upload/firmware", HTTP_POST, TrampolineUploadFirmware);
  reg("/api/factory_reset", HTTP_POST, TrampolineFactoryReset);
  reg("/api/coredump", HTTP_GET, TrampolineCoredumpGet);
  reg("/api/coredump", HTTP_DELETE, TrampolineCoredumpDelete);
  // Standalone heap_trace control surface — used to attribute slow
  // internal-heap drift to a specific allocator on a live device.
  // start?cap=N initializes a HEAP_TRACE_LEAKS recording with N records
  // in PSRAM; stop returns the unfreed-records dump as JSON for
  // addr2line postprocessing. Auth-gated; never registered on a
  // build that doesn't enable CONFIG_HEAP_TRACING_STANDALONE.
#if CONFIG_HEAP_TRACING_STANDALONE
  reg("/api/diag/heap_trace/start", HTTP_POST, TrampolineHeapTraceStart);
  reg("/api/diag/heap_trace/stop", HTTP_POST, TrampolineHeapTraceStop);
#endif

  // Long-lived SSE stream for the WebUI's live-refresh. Registered
  // via SseServer::RegisterRoute so the handler owns its own client
  // list + heartbeat task. Added to the URI handler count — the
  // max_uri_handlers bump above (48) comfortably covers it.
  if (sse_) {
    const esp_err_t sse_err = sse_->RegisterRoute(server_);
    if (sse_err != ESP_OK) {
      ESP_LOGW(kTag, "sse RegisterRoute failed: %s", esp_err_to_name(sse_err));
    }
  }
  // CORS preflights. Browsers send OPTIONS before any non-simple
  // cross-origin request (Content-Type: application/json qualifies).
  reg("/api/*", HTTP_OPTIONS, TrampolineOptions);

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
esp_err_t ControlServer::TrampolineShowText(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleShowText(req);
}
esp_err_t ControlServer::TrampolineShowCustom(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleShowCustom(req);
}
esp_err_t ControlServer::TrampolineScreenNext(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleScreenNext(req);
}
esp_err_t ControlServer::TrampolineScreenPrev(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleScreenPrev(req);
}
esp_err_t ControlServer::TrampolineStopDataSources(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleStopDataSources(req);
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
esp_err_t ControlServer::TrampolineLightsEffect(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleLightsEffect(req);
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
esp_err_t ControlServer::TrampolineActionSimulateZap(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleActionSimulateZap(req);
}
esp_err_t ControlServer::TrampolineActionClearPoolLogos(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleActionClearPoolLogos(req);
}
esp_err_t ControlServer::TrampolineFirmwareAutoUpdate(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)
      ->HandleFirmwareAutoUpdate(req);
}
esp_err_t ControlServer::TrampolineUploadFirmware(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleUploadFirmware(req);
}
esp_err_t ControlServer::TrampolineFactoryReset(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleFactoryReset(req);
}
esp_err_t ControlServer::TrampolineCoredumpGet(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleCoredumpGet(req);
}
esp_err_t ControlServer::TrampolineCoredumpDelete(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleCoredumpDelete(req);
}
esp_err_t ControlServer::TrampolineHeapTraceStart(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleHeapTraceStart(req);
}
esp_err_t ControlServer::TrampolineHeapTraceStop(httpd_req_t* req) {
  return static_cast<ControlServer*>(req->user_ctx)->HandleHeapTraceStop(req);
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
  if (!RequireHttpAuth(req)) return ESP_OK;
  const std::string body = BuildStatusJson();
  if (body.empty()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }
  return SendJson(req, body);
}

void ControlServer::BroadcastStatus() {
  if (!sse_) return;
  // Skip the allocation + serialisation altogether when no clients
  // are listening — a hot-loop DataHub push with nobody connected
  // would otherwise hammer the allocator.
  if (sse_->ClientCount() == 0) return;
  const std::string body = BuildStatusJson();
  if (body.empty()) return;
  sse_->Broadcast("status", body);
}

void ControlServer::SetCurrencies(std::vector<std::string> currencies) {
  cfg_.currencies = std::move(currencies);
  // Mirror the slot_count baseline computed in the constructor so
  // /api/status and /api/show/currency see the new shape immediately.
  std::lock_guard<std::mutex> lk(status_mu_);
  status_.slot_count =
      cfg_.currencies.empty()
          ? 1
          : static_cast<int32_t>(1 + 2 * cfg_.currencies.size());
}

std::string ControlServer::BuildStatusJson() const {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};

  LiveStatus live;
  {
    std::lock_guard<std::mutex> lk(status_mu_);
    live = status_;
  }

  // `currentScreen` is the settings-catalog `screens[].id` (api_id) so
  // the WebUI's ScreenButtons can compare it against the button it
  // rendered (status.currentScreen === s.id). When no slot_to_api_id
  // hook is wired we fall back to the raw slot — matches legacy
  // behaviour for tests / host-only builds.
  const int32_t current_api_id =
      cfg_.slot_to_api_id
          ? cfg_.slot_to_api_id(static_cast<size_t>(live.current_slot))
          : live.current_slot;
  cJSON_AddNumberToObject(root, "currentScreen", current_api_id);
  // `numScreens` is the hardware EPD panel count, not the rotation
  // slot count. Matches v3 (`root["numScreens"] = NUM_SCREENS;`) and
  // GET /api/settings, which the WebUI uses as `maxlength` for the
  // "show custom text" input — it must be the panel count, not the
  // filtered rotation length (which varies with feature flags). Using
  // live.slot_count here caused /api/status.numScreens to drift from
  // /api/settings.numScreens on any board with enabled currencies.
  cJSON_AddNumberToObject(root, "numScreens",
                          static_cast<double>(cfg_.num_screens));
  // `timerRunning` mirrors the old firmware's isTimerActive() — false
  // while the screen-rotation pause is armed. Prefer the real timer
  // iface when plumbed so this stays accurate even if the main loop
  // hasn't had a chance to re-publish LiveStatus yet.
  const bool timer_running =
      cfg_.timer ? !cfg_.timer->IsPaused() : live.timer_running;
  cJSON_AddBoolToObject(root, "timerRunning", timer_running);
  cJSON_AddBoolToObject(root, "isOTAUpdating", GetOtaManager().IsUpdating());

  const int64_t uptime_s = esp_timer_get_time() / 1000000;
  cJSON_AddNumberToObject(root, "espUptime", static_cast<double>(uptime_s));
  cJSON_AddBoolToObject(root, "epdInverted", btclock::epd::GetGlobalInverted());
  // "espFreeHeap"/"espHeapSize" describe INTERNAL SRAM only; PSRAM is
  // split into espFreePsram/espPsramSize so the free <= size invariant
  // holds. See heap_metrics.hpp for the full field contract.
  const std::size_t psram_free_bytes =
      esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                                 : 0;
  const std::size_t psram_total_bytes =
      esp_psram_is_initialized() ? esp_psram_get_size() : 0;
  AttachHeapMetricsJson(
      root, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL), psram_free_bytes,
      psram_total_bytes, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

  cJSON* conn = cJSON_AddObjectToObject(root, "connectionStatus");
  // Each channel reports its real upstream state when a callback is
  // wired (dataSource=1 plumbs price→Kraken, blocks→mempool, V2→false;
  // dataSource=0 plumbs all three to the v2 WS state). Without a
  // callback the field falls back to "hub is wired" — approximately
  // right only on the v2 path; bd btclock_v4-1xc tracks fully phasing
  // the heuristic out as remaining sources gain liveness probes.
  const bool fallback_up = cfg_.hub != nullptr;
  cJSON_AddBoolToObject(
      conn, "price",
      cfg_.price_connected ? cfg_.price_connected() : fallback_up);
  cJSON_AddBoolToObject(
      conn, "blocks",
      cfg_.blocks_connected ? cfg_.blocks_connected() : fallback_up);
  cJSON_AddBoolToObject(conn, "V2",
                        cfg_.v2_connected ? cfg_.v2_connected() : fallback_up);
  // Nostr tracks the zap-relay WebSocket's live state when wired. An
  // unset provider means the listener isn't configured (nostrZapNotify
  // off or relay URL blank) — report false in that case so the WebUI
  // badge correctly shows "not connected" rather than an implicit "N/A".
  cJSON_AddBoolToObject(conn, "nostr",
                        cfg_.nostr_connected ? cfg_.nostr_connected() : false);

  cJSON_AddNumberToObject(root, "rssi", CurrentRssi());
  cJSON_AddStringToObject(root, "currency",
                          live.currency.empty() ? "" : live.currency.c_str());

  // `data` — per-panel content. main.cpp refreshes `panel_texts` on
  // every render via ScreenManager::last_panel_texts(); we pad with
  // empty strings when the mirror hasn't caught up (e.g. first boot
  // before the first successful render).
  cJSON* data = cJSON_AddArrayToObject(root, "data");
  for (size_t i = 0; i < cfg_.num_screens; ++i) {
    const char* s =
        (i < live.panel_texts.size()) ? live.panel_texts[i].c_str() : "";
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
  std::snprintf(buf, sizeof(buf), "%u:%02u", static_cast<unsigned>(ds.end_hour),
                static_cast<unsigned>(ds.end_minute));
  cJSON_AddStringToObject(dnd, "endTime", buf);
  cJSON_AddBoolToObject(dnd, "active", ds.active);

  // Ambient-light sensor reading. Gated on "do we have a valid recent
  // reading" (adapter's IsAvailable()) rather than on a build-time
  // BTCLOCK_BOARD macro — boards that shipped with a BH1750 footprint but
  // no part soldered still report no lux, and that stays consistent
  // with /api/settings' `hasLightLevel` which feeds off the same hook.
  // Rev A / V8 wire light_sensor=nullptr so the field is suppressed.
  const bool light_available =
      cfg_.light_sensor && cfg_.light_sensor->IsAvailable();
  const float light_lux = light_available ? cfg_.light_sensor->GetLux() : -1.0f;
  AttachLightLevelJson(root, light_available, light_lux);

  // LEDs — mirror the per-pixel state, same shape /api/lights/status
  // returns. BuildLightsStatusArray reverses the index order to match
  // the old firmware's numPixels-i-1 convention.
  if (cfg_.leds) {
    const LedsIface::Status st = cfg_.leds->GetStatus();
    cJSON* leds = BuildLightsStatusArray(st);
    if (leds)
      cJSON_AddItemToObject(root, "leds", leds);
    else
      cJSON_AddItemToObject(root, "leds", cJSON_CreateArray());
  } else {
    cJSON_AddItemToObject(root, "leds", cJSON_CreateArray());
  }

  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) return {};
  std::string out(txt);
  free(txt);
  return out;
}

esp_err_t ControlServer::HandleSystemStatus(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }

  // Same internal-vs-PSRAM contract as BuildStatusJson — the helper
  // guarantees free <= size per pool and prevents the two endpoints
  // from drifting.
  const std::size_t psram_free =
      esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                                 : 0;
  const std::size_t psram_total =
      esp_psram_is_initialized() ? esp_psram_get_size() : 0;
  AttachHeapMetricsJson(
      root, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL), psram_free, psram_total,
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

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

  // Per-queue drop counters. Monotonic since boot. Non-zero values
  // indicate a wedged consumer (or a sustained burst that exceeded
  // the queue depth) — useful for triaging "buttons feel
  // unresponsive" / "frontlight stuck" reports without serial access.
  cJSON* drops = cJSON_AddObjectToObject(root, "queueDrops");
  if (drops) {
    cJSON_AddNumberToObject(drops, "buttons",
                            static_cast<double>(queue_metrics::GetDrops(
                                queue_metrics::Queue::kButtons)));
    cJSON_AddNumberToObject(drops, "led",
                            static_cast<double>(queue_metrics::GetDrops(
                                queue_metrics::Queue::kLed)));
    cJSON_AddNumberToObject(drops, "frontlight",
                            static_cast<double>(queue_metrics::GetDrops(
                                queue_metrics::Queue::kFrontlight)));
  }

  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleFullRefresh(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  ControlCommand cmd{ControlCommand::Kind::kFullRefresh};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleIdentify(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  ControlCommand cmd{ControlCommand::Kind::kIdentify};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleRestart(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Old firmware flushes the response body before calling esp_restart
  // via a delayed task — the connection has to close cleanly or the
  // WebUI gets a TCP-reset error. Do the same: respond first, post
  // kRestart command, main will run esp_restart after a short delay.
  SendEmptyOk(req);
  ControlCommand cmd{ControlCommand::Kind::kRestart};
  PostCommand(cmd);
  return ESP_OK;
}

esp_err_t ControlServer::HandleFactoryReset(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Require a small JSON body with an explicit confirmation token so a
  // stray POST (curl typo, CSRF, a router probing the LAN) can't wipe
  // NVS. The exact string "ERASE" was picked because it's short enough
  // to type by hand and different from any other control surface so
  // the WebUI can document it verbatim.
  constexpr std::size_t kMaxBody = 256;
  char* body = ReadFullBody(req, kMaxBody);
  std::string body_str = body ? std::string(body) : std::string();
  heap_caps_free(body);

  cJSON* root = cJSON_Parse(body_str.c_str());
  const cJSON* confirm =
      root ? cJSON_GetObjectItemCaseSensitive(root, "confirm") : nullptr;
  const bool ok = confirm && cJSON_IsString(confirm) && confirm->valuestring &&
                  std::string(confirm->valuestring) == "ERASE";
  cJSON_Delete(root);

  if (!ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"confirmation required\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }

  if (!cfg_.on_factory_reset) {
    // No handler wired up (e.g. AP-mode boot that never gets here, or a
    // unit-test harness). Surface as 503 rather than silently 200 so
    // the user knows nothing happened.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"factory reset not wired\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }

  // Send the response BEFORE running the callback — the callback
  // renders the splash and then calls PerformFactoryReset() which
  // reboots and never returns. If we sent after, the client would see
  // a TCP reset instead of a clean response.
  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char kBody[] = "{\"msg\":\"factory reset scheduled\"}";
  httpd_resp_send(req, kBody, sizeof(kBody) - 1);

  cfg_.on_factory_reset();
  // The callback doesn't return — PerformFactoryReset() is [[noreturn]].
  // Ret statement is here just to keep the control-flow analyser happy.
  return ESP_OK;
}

// --- /api/coredump GET + DELETE ------------------------------------
// Pull the panic backtrace from the previous run off the device. ELF
// stream so the client can pipe straight into espcoredump.py:
//   curl http://<ip>/api/coredump > dump.elf
//   espcoredump.py info_corefile -c dump.elf build-rev-b/btclock_v4.elf
// 404 when the partition is empty (the common case — most boots are
// clean). DELETE clears the partition so the next panic isn't ignored
// because a stale dump is occupying the slot. esp_core_dump_image_get
// returns the ELF region inside the partition; we read it directly via
// esp_partition_read so the response stays a contiguous byte stream
// rather than building it in heap.
esp_err_t ControlServer::HandleCoredumpGet(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;

  size_t addr = 0;
  size_t size = 0;
  if (esp_core_dump_image_check() != ESP_OK ||
      esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"no coredump present\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "coredump partition missing");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"coredump.elf\"");

  // 1536 B chunks match the static-file streamer above — same stack
  // budget on the httpd worker. Offset returned by image_get is
  // partition-absolute; subtract part->address to get the partition-
  // relative offset esp_partition_read expects.
  constexpr size_t kChunk = 1536;
  uint8_t buf[kChunk];
  size_t remaining = size;
  size_t offset = addr - part->address;
  while (remaining > 0) {
    const size_t n = remaining < kChunk ? remaining : kChunk;
    const esp_err_t rc = esp_partition_read(part, offset, buf, n);
    if (rc != ESP_OK) {
      ESP_LOGE(kTag, "coredump partition_read failed: %s", esp_err_to_name(rc));
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), n) !=
        ESP_OK) {
      // Client gone — return ESP_FAIL so esp_http_server skips its own
      // response.
      return ESP_FAIL;
    }
    offset += n;
    remaining -= n;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t ControlServer::HandleCoredumpDelete(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  const esp_err_t rc = esp_core_dump_image_erase();
  // ESP_ERR_NOT_FOUND means the partition was already empty — treat as
  // idempotent success so the WebUI's "Clear" button doesn't error on
  // the first click after a clean boot.
  if (rc != ESP_OK && rc != ESP_ERR_NOT_FOUND) {
    ESP_LOGE(kTag, "coredump erase failed: %s", esp_err_to_name(rc));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erase failed");
    return ESP_FAIL;
  }
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, nullptr, 0);
}

#if CONFIG_HEAP_TRACING_STANDALONE
namespace {
// Single-trace state. heap_trace allows only one active recording at a
// time, and the records buffer is sized once per start. Storing the
// pointer at TU scope (not in ControlServer) so the trace outlives the
// HTTP request callback that started it — the user runs start, exercises
// the suspect path for ~60 s, then runs stop. Records live in PSRAM so
// the trace itself doesn't pollute the internal heap we're measuring.
heap_trace_record_t* g_trace_records = nullptr;
size_t g_trace_capacity = 0;
bool g_trace_running = false;
}  // namespace
#endif  // CONFIG_HEAP_TRACING_STANDALONE

esp_err_t ControlServer::HandleHeapTraceStart(httpd_req_t* req) {
#if !CONFIG_HEAP_TRACING_STANDALONE
  httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED,
                      "heap_trace not built in");
  return ESP_FAIL;
#else
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Capacity bounded: each record is ~ (8 + 4*depth*4) bytes; 4096
  // records at depth=4 is ~150 KiB in PSRAM, fine. Default 256 keeps
  // the dump JSON cheap for the common case.
  size_t cap = 256;
  char buf[16];
  if (QueryParam(req, "cap", buf, sizeof(buf))) {
    const int n = std::atoi(buf);
    if (n > 0 && n <= 4096) cap = static_cast<size_t>(n);
  }

  if (g_trace_running) {
    heap_trace_stop();
    g_trace_running = false;
  }
  if (g_trace_records) {
    heap_caps_free(g_trace_records);
    g_trace_records = nullptr;
    g_trace_capacity = 0;
  }

  const size_t bytes = cap * sizeof(heap_trace_record_t);
  g_trace_records = static_cast<heap_trace_record_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g_trace_records) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "psram alloc failed");
    return ESP_FAIL;
  }
  esp_err_t err = heap_trace_init_standalone(g_trace_records, cap);
  if (err != ESP_OK) {
    heap_caps_free(g_trace_records);
    g_trace_records = nullptr;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "heap_trace_init_standalone failed");
    return ESP_FAIL;
  }
  err = heap_trace_start(HEAP_TRACE_LEAKS);
  if (err != ESP_OK) {
    heap_caps_free(g_trace_records);
    g_trace_records = nullptr;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "heap_trace_start failed");
    return ESP_FAIL;
  }
  g_trace_running = true;
  g_trace_capacity = cap;

  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char body[80];
  const int n = std::snprintf(
      body, sizeof(body), "{\"started\":true,\"capacity\":%u,\"depth\":%d}",
      static_cast<unsigned>(cap), CONFIG_HEAP_TRACING_STACK_DEPTH);
  return httpd_resp_send(req, body, n);
#endif
}

esp_err_t ControlServer::HandleHeapTraceStop(httpd_req_t* req) {
#if !CONFIG_HEAP_TRACING_STANDALONE
  httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED,
                      "heap_trace not built in");
  return ESP_FAIL;
#else
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!g_trace_running) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char body[] = "{\"error\":\"no trace running\"}";
    httpd_resp_send(req, body, sizeof(body) - 1);
    return ESP_OK;
  }
  heap_trace_stop();
  g_trace_running = false;

  const size_t count = heap_trace_get_count();

  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char buf[256];
  int n = std::snprintf(
      buf, sizeof(buf), "{\"count\":%u,\"capacity\":%u,\"leaks\":[",
      static_cast<unsigned>(count), static_cast<unsigned>(g_trace_capacity));
  httpd_resp_send_chunk(req, buf, n);

  bool first = true;
  for (size_t i = 0; i < count; ++i) {
    heap_trace_record_t rec;
    if (heap_trace_get(i, &rec) != ESP_OK) continue;
    // HEAP_TRACE_LEAKS auto-prunes freed records, so anything still in
    // the buffer is a live leak. Defensive freed_by check anyway.
    if (rec.freed_by[0] != nullptr) continue;
    int len = std::snprintf(buf, sizeof(buf),
                            "%s{\"sz\":%u,\"addr\":\"%p\",\"cc\":%u,\"pcs\":[",
                            first ? "" : ",", static_cast<unsigned>(rec.size),
                            rec.address, static_cast<unsigned>(rec.ccount));
    for (int j = 0; j < CONFIG_HEAP_TRACING_STACK_DEPTH; ++j) {
      if (rec.alloced_by[j] == nullptr) break;
      len += std::snprintf(buf + len, sizeof(buf) - len, "%s\"%p\"",
                           j == 0 ? "" : ",", rec.alloced_by[j]);
    }
    len += std::snprintf(buf + len, sizeof(buf) - len, "]}");
    httpd_resp_send_chunk(req, buf, len);
    first = false;
  }

  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
#endif
}

esp_err_t ControlServer::HandleShowScreen(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  char buf[16];
  if (!QueryParam(req, "s", buf, sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing s");
    return ESP_FAIL;
  }
  // `s` is the settings-catalog `screens[].id` (api_id) — what the WebUI's
  // screen-picker buttons post (ScreenButtons.svelte). Translate to the
  // dense ScreenManager slot here so the rest of the pipeline keeps its
  // zero-based slot model. An unresolved api_id means the id isn't
  // currently in the rotation (e.g. caller sent a stale catalog entry);
  // return 400 rather than silently wrapping to slot 0.
  const int api_id = atoi(buf);
  // Capability gate: a WebUI built against a richer pool (e.g. Ocean)
  // still has the mining-pool-earnings button and the user could POST
  // it after reconfiguring to a solo pool. Reject with 409 Conflict so
  // the client can surface a clear "that pool doesn't publish payouts"
  // message — 400 would be a lie (the id itself is valid), 503 would
  // imply a transient outage.
  if (cfg_.screen_is_hidden && cfg_.screen_is_hidden(api_id)) {
    char body[96];
    const int n = std::snprintf(
        body, sizeof(body),
        "{\"error\":\"screen %d unavailable for active pool\"}", api_id);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body, n);
    return ESP_OK;
  }
  int slot = api_id;
  if (cfg_.api_id_to_slot) {
    slot = cfg_.api_id_to_slot(api_id);
    if (slot < 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown screen id");
      return ESP_FAIL;
    }
  }
  ControlCommand cmd{ControlCommand::Kind::kShowScreen};
  cmd.arg_i = slot;
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleShowCurrency(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
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

esp_err_t ControlServer::HandleShowText(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Accept either ?t=TEXT (old-firmware wire format, preserved by the
  // OneParamRewrite /api/show/text/{text} → ?t=) or a JSON body
  // {"text":"..."}. The query-param path takes precedence when both
  // are supplied so a URL-rewritten call can't be quietly silenced by
  // a stray request body.
  std::string text;
  {
    char qbuf[256];
    if (QueryParam(req, "t", qbuf, sizeof(qbuf))) {
      text.assign(qbuf);
    }
  }

  // Size the panel vector to the active board's panel count; ControlServer
  // doesn't know this directly, so we rely on cfg_.num_screens (set by
  // main.cpp from the NUM_SCREENS panel array size). This keeps the
  // split-across-panels heuristic aligned with what the renderer paints.
  const std::size_t n_panels = cfg_.num_screens ? cfg_.num_screens : 7;

  ShowTextParseResult parsed;
  if (!text.empty()) {
    // Query-param path — uppercase + one-char-per-panel, matching the
    // old firmware's onApiShowText. The pure parser takes JSON only, so
    // the split runs inline here.
    parsed.ok = true;
    parsed.cells.assign(n_panels, std::string());
    for (std::size_t i = 0; i < text.size() && i < n_panels; ++i) {
      const unsigned char u = static_cast<unsigned char>(text[i]);
      parsed.cells[i].assign(1,
                             static_cast<char>(u < 0x80 ? std::toupper(u) : u));
    }
  } else {
    // Fall back to JSON body. Bound it well below any realistic use
    // case; one-char-per-panel caps the useful payload at ~16 bytes,
    // but we leave room for whitespace + JSON punctuation + future
    // extensions. 1 KiB matches other /api/* POST bodies in this file.
    constexpr std::size_t kMaxBody = 1024;
    char* body = ReadFullBody(req, kMaxBody);
    std::string body_str = body ? std::string(body) : std::string();
    heap_caps_free(body);
    parsed = ParseShowTextBody(body_str, n_panels);
  }
  if (!parsed.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, parsed.error.c_str());
    return ESP_FAIL;
  }
  {
    std::lock_guard<std::mutex> lk(pending_custom_mu_);
    pending_custom_cells_ = std::move(parsed.cells);
    pending_custom_valid_ = true;
  }
  ControlCommand cmd{ControlCommand::Kind::kShowCustom};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleShowCustom(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Per-panel string array — authoritative exact layout. Old-firmware
  // actions.cpp:90 AsyncCallbackJsonWebHandler path reads the JSON
  // body as an array-of-strings and writes each entry straight into
  // EPDManager's content array. 8 KiB caps the body at ~1 KiB/panel
  // which is plenty for any reasonable label and still well under the
  // control-server's other JSON bounds.
  constexpr std::size_t kMaxBody = 8 * 1024;
  char* body = ReadFullBody(req, kMaxBody);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  std::string body_str(body);
  heap_caps_free(body);

  const std::size_t n_panels = cfg_.num_screens ? cfg_.num_screens : 7;
  ShowTextParseResult parsed = ParseShowCustomBody(body_str, n_panels);
  if (!parsed.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, parsed.error.c_str());
    return ESP_FAIL;
  }
  {
    std::lock_guard<std::mutex> lk(pending_custom_mu_);
    pending_custom_cells_ = std::move(parsed.cells);
    pending_custom_valid_ = true;
  }
  ControlCommand cmd{ControlCommand::Kind::kShowCustom};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleScreenNext(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  ControlCommand cmd{ControlCommand::Kind::kNextScreen};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleScreenPrev(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  ControlCommand cmd{ControlCommand::Kind::kPrevScreen};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleStopDataSources(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  ControlCommand cmd{ControlCommand::Kind::kStopDataSources};
  PostCommand(cmd);
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleRestartDataSources(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
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
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->On();
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightOff(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->Off();
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightFlash(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"frontlight not present on this board\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  cfg_.frontlight->Flash();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleFrontlightStatus(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"frontlight not present on this board\"}";
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
    cJSON_AddItemToArray(
        arr, cJSON_CreateNumber(static_cast<double>(st.current_duty)));
  }
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleFrontlightBrightness(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.frontlight) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"frontlight not present on this board\"}";
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
  BroadcastStatus();
  return SendEmptyOk(req);
}

// --- Lights handlers ---------------------------------------------
// The /api/lights/* surface mirrors the old Arduino firmware's
// src/lib/net/webserver/lights.cpp shapes 1:1 so the existing WebUI
// drives this port unchanged:
//
//   GET  /api/lights          -> [{"red":R,"green":G,"blue":B,"hex":"#RRGGBB"},
//   ...] POST /api/lights/color?c=RRGGBB  (or "off") -> same status body POST
//   /api/lights/off      -> 200 OK, empty body POST /api/lights/set      ->
//   body is a JSON array of per-pixel
//                                objects {"red":..,"green":..,"blue":..}
//                                or {"hex":"#RRGGBB"}.
//
// Each handler below runs RequireHttpAuth() up front so the lights
// surface is gated identically to the rest of /api/*.

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

// ReadFullBody hoisted to the top-of-file anonymous namespace; callers
// in this block use that definition.

}  // namespace

esp_err_t ControlServer::HandleLightsStatus(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
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
  if (!RequireHttpAuth(req)) return ESP_OK;
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
  BroadcastStatus();
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
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.leds) return SendLedsUnavailable(req);
  cfg_.leds->SetSolidColor(0);
  BroadcastStatus();
  // Old firmware returns 200 OK with empty body.
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleLightsSet(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.leds) return SendLedsUnavailable(req);
  constexpr size_t kMaxBody = 1024;
  char* body = ReadFullBody(req, kMaxBody);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  cJSON* root = cJSON_Parse(body);
  heap_caps_free(body);
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
    BroadcastStatus();
    return SendEmptyOk(req);
  }
  if (n != static_cast<int>(st_before.pixel_count)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pixel count mismatch");
    return ESP_FAIL;
  }
  uint32_t pixels[8] = {0};
  for (int i = 0;
       i < n && i < static_cast<int>(sizeof(pixels) / sizeof(pixels[0])); ++i) {
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
  BroadcastStatus();
  return SendEmptyOk(req);
}

// --- /api/lights/effect ------------------------------------------------
// JSON body `{"name": "<effect>"}`. Unknown JSON keys are silently
// ignored so callers can speculatively send `color`, `count`,
// `duration_ms` etc. and we wire those up later without breaking
// existing clients. The name table lives in `LedsAdapter` so the
// webserver TU stays decoupled from `io/led_controller.hpp`.
esp_err_t ControlServer::HandleLightsEffect(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.leds) return SendLedsUnavailable(req);

  constexpr std::size_t kMaxBody = 256;
  char* body = ReadFullBody(req, kMaxBody);
  std::string body_str = body ? std::string(body) : std::string();
  heap_caps_free(body);

  cJSON* root = cJSON_Parse(body_str.c_str());
  const cJSON* name_node =
      root ? cJSON_GetObjectItemCaseSensitive(root, "name") : nullptr;
  const bool has_name = name_node && cJSON_IsString(name_node) &&
                        name_node->valuestring && name_node->valuestring[0];
  if (!has_name) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"name required\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }

  // Copy the name out before releasing the cJSON tree — the response
  // echoes it back and we don't want a use-after-free if the writer
  // mutates the buffer between Delete and snprintf.
  std::string name = name_node->valuestring;
  cJSON_Delete(root);

  if (!cfg_.leds->PostEffectByName(name.c_str())) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"unknown effect\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }

  // PostEffectByName returned true, so `name` matched a table entry.
  // Every entry is a short lower-snake string under 16 chars; a 64 B
  // buffer is plenty and keeps the handler stack frame small.
  httpd_resp_set_type(req, kJsonType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char out[64];
  const int n = std::snprintf(
      out, sizeof(out), "{\"queued\":true,\"name\":\"%s\"}", name.c_str());
  return httpd_resp_send(req, out, n > 0 ? static_cast<size_t>(n) : 0);
}

// --- WiFi TX-power ------------------------------------------------
// Body is JSON `{"txPower": <quarter-dBm int>}`. Matches the units of
// esp_wifi_set_max_tx_power (int8_t quarter-dBm). Accepts a modest
// request size — the body is a single field.

esp_err_t ControlServer::HandleWifiTxPower(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  constexpr size_t kMaxBody = 128;
  if (req->content_len == 0 || req->content_len > kMaxBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  char body[kMaxBody + 1];
  int total = 0;
  while (total < static_cast<int>(req->content_len)) {
    const int r = httpd_req_recv(
        req, body + total,
        static_cast<size_t>(static_cast<int>(req->content_len) - total));
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
// Auth: gated by RequireHttpAuth() in HandleUploadWebui — once
// httpAuthEnabled is true an unauthenticated POST here can't flash
// anything.

namespace {

// Trampoline for FlashWebuiImage / OTA push — expects a C-style callback.
// `ctx` is the httpd_req_t*; read up to `want` bytes into `buf`.
//
// httpd_req_recv returns HTTPD_SOCK_ERR_TIMEOUT (-3) after the httpd
// config's recv_wait_timeout (default 5 s) with no socket data. On a
// slow/contended WiFi link that happens several times during a 1–2 MB
// upload even when the client is healthy; the old firmware treated
// these as fatal, which is why OTA "works sometimes." Retry a few
// times before giving up so a transient stall doesn't abort a
// full-partition erase mid-stream.
constexpr int kHttpdRecvTimeoutRetries = 6;  // ~30 s of silence

int HttpdRecvTrampoline(void* ctx, char* buf, size_t want) {
  auto* req = static_cast<httpd_req_t*>(ctx);
  for (int attempt = 0; attempt <= kHttpdRecvTimeoutRetries; ++attempt) {
    const int r = httpd_req_recv(req, buf, want);
    if (r != HTTPD_SOCK_ERR_TIMEOUT) return r;
    ESP_LOGW(kTag, "httpd_req_recv timeout (attempt %d/%d)", attempt + 1,
             kHttpdRecvTimeoutRetries + 1);
  }
  return HTTPD_SOCK_ERR_TIMEOUT;
}

// Deferred reboot callback. Scheduled with a short delay so the HTTP
// response has flushed to the client's socket before the reboot fires;
// otherwise the client sees a TCP-reset and surfaces a generic error
// instead of the "reboot scheduled" body we sent.
void RebootTimerCallback(void* /*arg*/) {
  esp_restart();
}

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
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!RequireOtaEnabled(req)) return ESP_OK;
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
    std::snprintf(body, sizeof(body), "{\"error\":\"oversize\",\"max\":%u}",
                  static_cast<unsigned>(part_size));
    httpd_resp_send(req, body, strlen(body));
    return ESP_FAIL;
  }

  ESP_LOGW(kTag, "webui upload starting: content-length=%u partition=%u",
           static_cast<unsigned>(expected), static_cast<unsigned>(part_size));

  size_t written = 0;
  const esp_err_t rc =
      btclock::FlashWebuiImage(&HttpdRecvTrampoline, req, expected, &written);

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
  std::snprintf(body, sizeof(body), "{\"result\":\"ok\",\"bytes\":%u}",
                static_cast<unsigned>(written));
  SendJson(req, body);
  ScheduleReboot(500);
  return ESP_OK;
}

// --- Firmware OTA handlers ----------------------------------------
// Pull-OTA: kick off a background task that does the release-JSON
// lookup, downloads the asset, verifies SHA-256, and reboots on
// success. The HTTP response comes back immediately — the update is
// observed via /api/status `isOTAUpdating`.

esp_err_t ControlServer::HandleFirmwareAutoUpdate(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!RequireOtaEnabled(req)) return ESP_OK;
  const esp_err_t rc = GetOtaManager().TriggerAutoUpdate();
  if (rc == ESP_ERR_INVALID_STATE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"msg\":\"Update already in progress\"}";
    return httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  }
  if (rc != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(rc));
    return ESP_FAIL;
  }
  return SendJson(req, "{\"msg\":\"Firmware update triggered\"}");
}

// Push-OTA: mirrors HandleUploadWebui's body-streaming pattern but
// targets the next OTA app partition via OtaManager.
esp_err_t ControlServer::HandleUploadFirmware(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!RequireOtaEnabled(req)) return ESP_OK;
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no ota partition");
    return ESP_FAIL;
  }

  const size_t expected = req->content_len;
  if (!btclock::IsValidFirmwareUploadSize(expected, next->size)) {
    if (expected == 0) {
      // Missing / zero Content-Length — the body might still stream
      // but the old behaviour of "read until the partition fills up"
      // either spins for minutes on a closed socket or misreads past
      // the image. Require a declared length.
      httpd_resp_set_status(req, "411 Length Required");
      httpd_resp_set_type(req, kJsonType);
      httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
      const char kBody[] = "{\"error\":\"content-length required\"}";
      httpd_resp_send(req, kBody, sizeof(kBody) - 1);
      return ESP_FAIL;
    }
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char body[96];
    std::snprintf(body, sizeof(body), "{\"error\":\"oversize\",\"max\":%u}",
                  static_cast<unsigned>(next->size));
    httpd_resp_send(req, body, strlen(body));
    return ESP_FAIL;
  }

  // Optional ?sha256=<64-hex>. When present the upload is rejected
  // unless the streamed bytes hash to the supplied digest.
  char sha_buf[80] = {};
  const bool have_sha = QueryParam(req, "sha256", sha_buf, sizeof(sha_buf));

  ESP_LOGW(kTag, "firmware upload starting: content-length=%u partition=%u",
           static_cast<unsigned>(expected), static_cast<unsigned>(next->size));

  size_t written = 0;
  const esp_err_t rc =
      GetOtaManager().WritePushImage(&HttpdRecvTrampoline, req, expected,
                                     have_sha ? sha_buf : nullptr, &written);

  if (rc == ESP_ERR_INVALID_STATE) {
    // Already updating — pull-OTA is in progress, or a previous push
    // hasn't released the flag yet. 503 matches the pull-OTA response.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"msg\":\"Update already in progress\"}";
    httpd_resp_send(req, kBody, sizeof(kBody) - 1);
    return ESP_FAIL;
  }
  if (rc == ESP_ERR_INVALID_CRC) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char kBody[] = "{\"error\":\"sha256 mismatch\"}";
    httpd_resp_send(req, kBody, sizeof(kBody) - 1);
    return ESP_FAIL;
  }
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "firmware upload failed: %s", esp_err_to_name(rc));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(rc));
    return ESP_FAIL;
  }

  ESP_LOGW(kTag, "firmware upload ok: bytes=%u",
           static_cast<unsigned>(written));
  char body[96];
  std::snprintf(body, sizeof(body), "{\"result\":\"ok\",\"bytes\":%u}",
                static_cast<unsigned>(written));
  SendJson(req, body);
  // Completion blink runs AFTER the HTTP body has flushed so the
  // client's connection doesn't wait on the LED animation. It blocks
  // the httpd worker for ~1 s; the reboot delay below is widened past
  // that so the scheduled esp_restart doesn't fire mid-blink.
  if (cfg_.on_ota_completion_blink) cfg_.on_ota_completion_blink();
  ScheduleReboot(1500);
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
// assets don't reveal any state beyond their shipped bytes. The first
// /api/* call the page makes triggers the 401 + Basic prompt, which
// is the correct place for the browser credential dialog.

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
  // MimeTypeForPath always returns a string literal (see mime.hpp), so
  // .data() is null-terminated and outlives the response. Constructing
  // a temporary std::string here hands httpd a dangling pointer whose
  // memory gets overwritten by the body-chunk buffer below — the symptom
  // is Content-Type showing bytes from the gzip FNAME field.
  httpd_resp_set_type(req, MimeTypeForPath(rel).data());
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

// Trim ASCII whitespace from a string in-place. Used after reading
// commit.txt — the WebUI's gzip_build.py appends a trailing newline.
void RtrimWhitespace(std::string* s) {
  while (!s->empty() && (s->back() == '\n' || s->back() == '\r' ||
                         s->back() == ' ' || s->back() == '\t')) {
    s->pop_back();
  }
}

// Read /lfs/www/manifest.json and return its `commit` field, or empty
// on parse failure. Format produced by data/gzip_build.py:
//   {"commit": "<sha>", "minFirmware": "X.Y.Z", "buildTime": <unix>}
std::string ReadCommitFromManifest() {
  FILE* f = std::fopen("/lfs/www/manifest.json", "rb");
  if (!f) return "";
  // 256 bytes is well above the actual manifest size (~120) and stays
  // off the FreeRTOS task stack — webserver task budget is tight.
  char buf[256];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  cJSON* root = cJSON_Parse(buf);
  if (!root) return "";
  std::string out;
  const cJSON* commit = cJSON_GetObjectItemCaseSensitive(root, "commit");
  if (cJSON_IsString(commit) && commit->valuestring != nullptr) {
    out = commit->valuestring;
  }
  cJSON_Delete(root);
  RtrimWhitespace(&out);
  return out;
}

// Backwards-compat: older WebUI images shipped a plain commit.txt at
// the LittleFS root (under /lfs/www/) instead of a manifest.json.
std::string ReadCommitFromCommitTxt() {
  FILE* f = std::fopen("/lfs/www/commit.txt", "rb");
  if (!f) return "";
  char buf[64];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  std::string out(buf, n);
  RtrimWhitespace(&out);
  return out;
}

// Prefer manifest.json; fall back to commit.txt for older LittleFS
// images flashed against newer firmware.
std::string ReadWebuiCommitFromLittleFs() {
  std::string c = ReadCommitFromManifest();
  if (!c.empty()) return c;
  return ReadCommitFromCommitTxt();
}

// Populate a settings::DeviceContext from runtime state. Kept small
// and side-effect free so the GET path doesn't have to touch NVS for
// facts like hwRev or firmware git info.
btclock::settings::DeviceContext BuildDeviceContext(
    const ControlServer::Config& cfg) {
  btclock::settings::DeviceContext ctx;
  // Wifi component doesn't yet surface the configured hostname — the
  // old firmware built it from hostnamePrefix + last-3-MAC-bytes.
  // Route through the shared helper so this field matches the mDNS
  // advertisement exactly; earlier revisions used a 4-hex-char suffix
  // here while mDNS used 6, so the WebUI reported a name nobody could
  // actually ping.
  {
    btclock::Prefs p("settings");
    const std::string prefix = p.GetString("hostnamePrefix", "btclock");
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ctx.hostname = btclock::net_util::ComputeHostname(prefix, mac);
  }
  ctx.ip = cfg.wifi ? cfg.wifi->ip() : "";
  int8_t tx = 0;
  esp_wifi_get_max_tx_power(&tx);
  ctx.tx_power = tx;
  ctx.num_screens = static_cast<int32_t>(cfg.num_screens);
  ctx.has_frontlight = cfg.frontlight != nullptr;
  // Sensor-present gate: absent (nullptr adapter) OR present-but-uninit
  // (Init() failed / sensor unsoldered) both report as "no lux". The
  // adapter's IsAvailable() folds both cases so we don't have to.
  if (cfg.light_sensor && cfg.light_sensor->IsAvailable()) {
    ctx.has_light_level = true;
    ctx.light_level = cfg.light_sensor->GetLux();
  } else {
    ctx.has_light_level = false;
  }
  // Prefer the machine-readable id (matches the WebUI's
  // firmwareBinaryMap / webuiBinaryMap keys); fall back to the
  // display name only when the call site forgot to populate it.
  ctx.hw_rev = cfg.hw_id.empty() ? cfg.hw_name : cfg.hw_id;
  // Read the WebUI's git rev from /lfs/www/. Prefer manifest.json (the
  // new format — also carries the WebUI/firmware compatibility contract
  // via `minFirmware`), fall back to commit.txt for older LittleFS
  // images. Both are written by data/gzip_build.py. Reading on every
  // GET rather than caching at boot means an OTA-flashed LittleFS
  // image is reflected immediately without a reboot. Files are ~120
  // bytes and live in the same partition the static handler is already
  // hitting, so the cost is negligible. Empty when both files are
  // missing; settings_api.cpp suppresses the field on empty so the
  // WebUI renders a skeleton rather than a stale value.
  ctx.fs_rev = ReadWebuiCommitFromLittleFs();
  // `gitTag` is non-empty only when the firmware repo's HEAD is exactly
  // on a release tag (see components/webserver/CMakeLists.txt). v3
  // surfaced this as the SystemInfo "Version" row; dev builds get an
  // empty string and the WebUI hides the row entirely.
#ifdef BTCLOCK_GIT_TAG
  ctx.git_tag = BTCLOCK_GIT_TAG;
#endif
  const esp_app_desc_t* desc = esp_app_get_description();
  if (desc) {
    ctx.git_rev = desc->version;
    // desc->date is `MMM DD YYYY` (same as __DATE__) and desc->time is
    // `HH:MM:SS` — both treated as UTC. Parse once per GET; the result
    // is a small integer so recomputing it is cheaper than caching.
    ctx.last_build_time_unix =
        btclock::settings::ParseCompilerBuildTimeUnix(desc->date, desc->time);
  }
  ctx.available_fonts = cfg.available_fonts;
  ctx.available_pools = cfg.available_pools;
  ctx.available_currencies = cfg.available_currencies;
  // Feature-flag gates for the settings page's `screens[]`. Read fresh
  // from NVS so a PATCH toggling them takes effect on the very next GET
  // without needing a reboot or a cache invalidation — matches the same
  // read-every-request shape used by `screen_is_hidden` below.
  {
    btclock::Prefs p(btclock::prefs::kSettingsNs);
    ctx.mining_pool_stats_enabled =
        btclock::settings::ReadBool(p, prefs::kMiningPoolStats);
    ctx.bitaxe_enabled = btclock::settings::ReadBool(p, prefs::kBitaxeEnabled);
  }
  for (const auto& s : cfg.screens_catalog) {
    ctx.screens.push_back({s.id, s.name});
    // Let the suppression probe filter capability-gated slots — today
    // this is just mining-pool earnings on a solo pool. Evaluated at
    // request time so a PATCH changing `miningPoolName` takes effect on
    // the next GET without a reboot.
    if (cfg.screen_is_hidden && cfg.screen_is_hidden(s.id)) {
      ctx.hidden_screen_ids.push_back(s.id);
    }
  }
  return ctx;
}

}  // namespace

esp_err_t ControlServer::HandleSettingsGet(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
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
  if (!RequireHttpAuth(req)) return ESP_OK;
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
    const int r = httpd_req_recv(
        req, body.data() + total,
        static_cast<size_t>(static_cast<int>(req->content_len) - total));
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
    std::snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", result.error.c_str());
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
  // other runtime consumers (screen rotation, timezone) will pick up
  // the new NVS values on their next read.
  //
  // DND is the exception: its suppressor predicate is evaluated every
  // frame against an in-memory cache in `dnd::Instance()`, so a bare
  // NVS write here would stay invisible until reboot reloads that
  // cache. Fire the configured hook so main.cpp can copy the fresh
  // schedule into the singleton and the LED/frontlight gates react on
  // the next tick.
  if (cfg_.on_dnd_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kDndEnabled ||
          k == btclock::prefs::kDndTimeEnabled ||
          k == btclock::prefs::kDndStartHour ||
          k == btclock::prefs::kDndStartMin ||
          k == btclock::prefs::kDndEndHour || k == btclock::prefs::kDndEndMin) {
        cfg_.on_dnd_changed();
        break;
      }
    }
  }

  // tzString: apply live so the clock screen follows the new zone
  // without reboot. Old firmware did the same — it called setenv/tzset
  // inside the settings-save handler rather than deferring to boot.
  if (cfg_.on_tz_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kTzString) {
        const std::string zone = prefs.GetString(btclock::prefs::kTzString, "");
        cfg_.on_tz_changed(zone);
        break;
      }
    }
  }

  // invertedColor: EPD polarity flips on the next render. The handler
  // below installs the flag on the driver + marks the screen dirty so
  // the user sees the colour swap within one frame. The touched-keys
  // list uses "invertedColor" (JSON field name), not the NVS key
  // (identical in this case), because ApplyPatch emplaces the JSON key.
  if (cfg_.on_inverted_color_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == "invertedColor") {
        cfg_.on_inverted_color_changed(
            prefs.GetBool(btclock::prefs::kInvertedColor, false));
        break;
      }
    }
  }

  // fontName: rebind the AppFonts role accessors and mark the screen
  // dirty so the next full refresh repaints with the new family. The
  // touched-keys list uses the JSON field name ("fontName"); the NVS
  // key happens to match. Nullable hook — if main.cpp didn't wire it,
  // the change takes effect at the next reboot.
  if (cfg_.on_font_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == "fontName") {
        cfg_.on_font_changed(
            prefs.GetString(btclock::prefs::kFontName, "antonio"));
        break;
      }
    }
  }

  // satsVariant: rebind the renderer's glyph index live. Schema
  // already range-clamped 0..15; cast is safe because GetU32 is
  // guaranteed to return a value within the kUint default for the
  // key, but ClampSatsVariant on the read side belt-and-braces it.
  if (cfg_.on_sats_variant_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kSatsVariant) {
        const uint32_t v = prefs.GetU32(btclock::prefs::kSatsVariant, 7);
        cfg_.on_sats_variant_changed(static_cast<uint8_t>(v & 0x0Fu));
        break;
      }
    }
  }

  // blockFlashColor: mirror the new value into the LED controller so
  // the next block flash uses the user-chosen colour without a reboot.
  // Both the LED controller and the settings layer now read the same
  // `settings` namespace, so this hook is purely a runtime cache poke
  // (the persisted value is already correct).
  if (cfg_.on_block_flash_color_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kBlockFlashColor) {
        cfg_.on_block_flash_color_changed(
            prefs.GetU32(btclock::prefs::kBlockFlashColor, 0xE04300));
        break;
      }
    }
  }

  // blockFeeDec PATCH hook — re-read the freshly-persisted NVS bool
  // and hand it to the v2 WS client so the fee-stream subscription
  // switches without reboot. Mirrors the on_block_flash_color hook
  // shape (single-key trigger, NVS read for the canonical value).
  if (cfg_.on_block_fee_dec_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kBlockFeeDec) {
        cfg_.on_block_fee_dec_changed(
            prefs.GetBool(btclock::prefs::kBlockFeeDec, true));
        break;
      }
    }
  }

  // ledBrightness / disableLeds / ledFlashOnUpd: same shape as
  // blockFlashColor — refresh the LED controller's in-memory state so
  // the change applies on the next effect without a reboot. Without
  // these hooks the NVS key is updated but the runtime cache the LED
  // task reads on each frame keeps the pre-PATCH value.
  if (cfg_.on_led_brightness_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kLedBrightness) {
        cfg_.on_led_brightness_changed(static_cast<uint8_t>(
            prefs.GetU32(btclock::prefs::kLedBrightness, 128) & 0xFFu));
        break;
      }
    }
  }
  if (cfg_.on_disable_leds_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kDisableLeds) {
        cfg_.on_disable_leds_changed(
            prefs.GetBool(btclock::prefs::kDisableLeds, false));
        break;
      }
    }
  }
  if (cfg_.on_led_flash_on_upd_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kLedFlashOnUpd) {
        cfg_.on_led_flash_on_upd_changed(
            prefs.GetBool(btclock::prefs::kLedFlashOnUpd, false));
        break;
      }
    }
  }

  // screenOrder / screen<id>Visible / actCurrencies: rebuild the
  // ScreenManager's rotation traversal so the new order / visibility /
  // currency-set takes effect on the next auto-rotate or
  // /api/screen/next without a reboot. Without this the sequence is
  // only built once at boot and a runtime PATCH writes NVS but leaves
  // the runtime walk stale. The screen<id>Visible keys end with
  // "Visible" (e.g. screen0Visible, screen10Visible) — suffix-match
  // keeps this independent of the catalogue id list. actCurrencies
  // also resizes per-currency screen slots so the same hook handles it.
  // miningPoolStats / bitaxeEnabled gate the child slots (70/71, 80/81)
  // so flipping them at runtime must rebuild rotation too — without
  // this, toggling either off leaves the dormant slots in the auto-
  // rotate cycle until reboot.
  if (cfg_.on_screens_changed) {
    for (const auto& k : result.touched_keys) {
      const bool is_order = (k == btclock::prefs::kScreenOrder);
      const bool is_visible = (k.size() > 7 && k.compare(0, 6, "screen") == 0 &&
                               k.compare(k.size() - 7, 7, "Visible") == 0);
      const bool is_currencies = (k == btclock::prefs::kActCurrencies);
      const bool is_feature_gate = (k == btclock::prefs::kMiningPoolStats ||
                                    k == btclock::prefs::kBitaxeEnabled);
      if (is_order || is_visible || is_currencies || is_feature_gate) {
        cfg_.on_screens_changed();
        break;
      }
    }
  }

  // Runtime-editable frontlight keys: re-read the canonical settings
  // namespace and push the new values into FrontlightController so a
  // PATCH lands without a reboot. Without this, init_hardware.cpp's
  // boot-time NVS read is the only sync point and luxLightToggle /
  // flOffWhenDark / flMaxBrightness / flEffectDelay / flAlwaysOn /
  // flDisable / flFlashOnUpd would each persist to NVS but never
  // reach the controller until restart. bd btclock_v4-7xv /
  // btclock_v4-63p.
  if (cfg_.on_frontlight_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kLuxLightToggle ||
          k == btclock::prefs::kFlOffWhenDark ||
          k == btclock::prefs::kFlMaxBrightness ||
          k == btclock::prefs::kFlEffectDelay ||
          k == btclock::prefs::kFlAlwaysOn || k == btclock::prefs::kFlDisable ||
          k == btclock::prefs::kFlFlashOnUpd) {
        cfg_.on_frontlight_changed();
        break;
      }
    }
  }

  // mdnsEnabled / hostnamePrefix: tear down + re-publish the mDNS
  // advert so the device responds under its new name (or disappears
  // when `mdnsEnabled` flips false) without a reboot. Without this
  // hook init_mdns ran exactly once at boot and a runtime PATCH wrote
  // NVS but the responder kept serving the stale hostname / TXT set.
  // bd btclock_v4-9ut.
  if (cfg_.on_mdns_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kMdnsEnabled ||
          k == btclock::prefs::kHostnamePrefix) {
        cfg_.on_mdns_changed();
        break;
      }
    }
  }

  // Runtime-editable nostr keys: rebuild the zap listener so the new
  // pubkey / gate values take effect without a reboot. nostrRelay and
  // nostrPubKey are boot_only in the schema and trigger the generic
  // rebootRequired response — they don't fire this hook (a live
  // RelayClient swap on the audited zap path isn't worth the
  // additional complexity given the existing reboot prompt). bd
  // btclock_v4-aw5 / btclock_v4-q1l.
  if (cfg_.on_nostr_changed) {
    for (const auto& k : result.touched_keys) {
      if (k == btclock::prefs::kNostrZapPubkey ||
          k == btclock::prefs::kNostrZapNotify ||
          k == btclock::prefs::kLedFlashOnZap ||
          k == btclock::prefs::kFlFlashOnZap ||
          k == btclock::prefs::kScrnRestoreZap) {
        cfg_.on_nostr_changed();
        break;
      }
    }
  }

  // Every successful PATCH fires the generic "settings saved" hook so
  // main can emit a visible confirmation (green LED pulse today). No
  // touched-keys filter — an empty-but-valid PATCH still counts as a
  // save in the user's mental model, and the LED flash is cheap.
  if (cfg_.on_settings_patched) cfg_.on_settings_patched();

  // Response body mirrors old firmware: 200 OK with an empty body
  // when no reboot is required, {"rebootRequired":true} otherwise.
  BroadcastStatus();
  if (result.reboot_required) {
    return SendJson(req, "{\"rebootRequired\":true}");
  }
  return SendEmptyOk(req);
}

// --- DND + timer-pause handlers ------------------------------------

esp_err_t ControlServer::HandleDndStatus(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
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
  std::snprintf(buf, sizeof(buf), "%u:%02u", static_cast<unsigned>(ds.end_hour),
                static_cast<unsigned>(ds.end_minute));
  cJSON_AddStringToObject(root, "endTime", buf);
  cJSON_AddBoolToObject(root, "active", ds.active);
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return SendJsonChar(req, txt);
}

esp_err_t ControlServer::HandleDndEnable(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.dnd) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no dnd");
    return ESP_FAIL;
  }
  cfg_.dnd->SetEnabled(true);
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleDndDisable(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.dnd) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no dnd");
    return ESP_FAIL;
  }
  cfg_.dnd->SetEnabled(false);
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionPause(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.timer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no timer");
    return ESP_FAIL;
  }
  const bool was_paused = cfg_.timer->IsPaused();
  cfg_.timer->SetPaused(true);
  if (!was_paused && cfg_.on_rotation_paused_changed) {
    cfg_.on_rotation_paused_changed(true);
  }
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionTimerRestart(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.timer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no timer");
    return ESP_FAIL;
  }
  // `timer_restart` in the old firmware re-arms the rotation clock,
  // which has the effect of resuming a paused run AND zeroing the
  // deadline so the next rotation is a full period away. Mirror that:
  // unpause first, then reset the deadline.
  const bool was_paused = cfg_.timer->IsPaused();
  cfg_.timer->SetPaused(false);
  cfg_.timer->Restart();
  if (was_paused && cfg_.on_rotation_paused_changed) {
    cfg_.on_rotation_paused_changed(false);
  }
  BroadcastStatus();
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionSimulateZap(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  if (!cfg_.simulate_zap) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "simulate_zap not wired");
    return ESP_FAIL;
  }
  // Defaults match the spec — small enough to land in a single-cell
  // amount but large enough to look credible during QA.
  int64_t amount_sats = 21000;
  std::string message = "test zap";

  // Body is optional. Empty / oversized / malformed bodies fall back
  // to defaults silently — the simulator is a dev/QA tool, not an
  // input-validation surface, so a bad body shouldn't 400.
  constexpr std::size_t kMaxBody = 1024;
  if (req->content_len > 0 && req->content_len <= kMaxBody) {
    if (char* body = ReadFullBody(req, kMaxBody)) {
      if (cJSON* root = cJSON_Parse(body)) {
        const cJSON* amt =
            cJSON_GetObjectItemCaseSensitive(root, "amount_sats");
        if (cJSON_IsNumber(amt)) {
          const double v = amt->valuedouble;
          if (v < 0.0)
            amount_sats = 0;
          else if (v > static_cast<double>(INT64_MAX))
            amount_sats = INT64_MAX;
          else
            amount_sats = static_cast<int64_t>(v);
        }
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
          message.assign(msg->valuestring);
          if (message.size() > 256) message.resize(256);
        }
        cJSON_Delete(root);
      }
      heap_caps_free(body);
    }
  }

  cfg_.simulate_zap(amount_sats, std::move(message));
  return SendEmptyOk(req);
}

esp_err_t ControlServer::HandleActionClearPoolLogos(httpd_req_t* req) {
  if (!RequireHttpAuth(req)) return ESP_OK;
  // Best-effort: clear what's there, log a count, never error out so
  // the WebUI button is harmless on a fresh device.
  const int n = btclock::pool_logos::ClearAllCached();
  // Re-enqueue a fetch for the active pool so the user doesn't have
  // to switch pools (or reboot) to repopulate the cache after a
  // deliberate clear. EnqueueFetch is a no-op when the pool has no
  // upstream logo (LookupMeta returns nullptr).
  {
    btclock::Prefs settings(btclock::prefs::kSettingsNs);
    const std::string active = btclock::settings::ReadString(
        settings, btclock::prefs::kMiningPoolName);
    if (!active.empty()) {
      (void)btclock::pool_logos::EnqueueFetch(active);
    }
  }
  char body[64];
  std::snprintf(body, sizeof(body), "{\"removed\":%d}", n < 0 ? 0 : n);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

}  // namespace btclock
