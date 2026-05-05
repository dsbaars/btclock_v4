#include "btclock_currencies_fetch.hpp"

#include <cstring>
#include <vector>

#include "btclock_currencies_parse.hpp"
#include "btclock_currencies_uri.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock-curr";

// Generous budget — the upstream payload is a tiny JSON array, but
// allow headroom in case the catalogue grows. 1 KiB is well below the
// typical heap budget at boot and bounds the worst case if a captive
// portal serves an HTML error page.
constexpr std::size_t kMaxBodyBytes = 1024;

struct FetchCtx {
  std::vector<char> buf;
  bool truncated = false;
};

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchCtx*>(evt->user_data);
  if (ctx == nullptr || evt->data == nullptr || evt->data_len <= 0) {
    return ESP_OK;
  }
  const std::size_t incoming = static_cast<std::size_t>(evt->data_len);
  if (ctx->buf.size() + incoming > kMaxBodyBytes) {
    ctx->truncated = true;
    return ESP_OK;
  }
  const std::size_t prev = ctx->buf.size();
  ctx->buf.resize(prev + incoming);
  std::memcpy(ctx->buf.data() + prev, evt->data, incoming);
  return ESP_OK;
}

}  // namespace

std::vector<std::string> FetchAvailableCurrencies(const std::string& ws_uri) {
  const std::string url = BuildCurrenciesUri(ws_uri);
  if (url.empty()) {
    ESP_LOGW(kTag, "skip: cannot derive HTTP URL from '%s'", ws_uri.c_str());
    return {};
  }

  FetchCtx ctx;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &HttpEvent;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGW(kTag, "esp_http_client_init failed for %s", url.c_str());
    return {};
  }

  std::vector<std::string> out;
  do {
    if (esp_http_client_perform(client) != ESP_OK) {
      ESP_LOGW(kTag, "perform failed for %s", url.c_str());
      break;
    }
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
      ESP_LOGW(kTag, "non-2xx (%d) from %s", status, url.c_str());
      break;
    }
    if (ctx.truncated || ctx.buf.empty()) {
      ESP_LOGW(kTag, "empty/truncated body from %s (truncated=%d size=%u)",
               url.c_str(), ctx.truncated ? 1 : 0,
               static_cast<unsigned>(ctx.buf.size()));
      break;
    }
    out = ParseCurrenciesJson(ctx.buf.data(), ctx.buf.size());
    if (out.empty()) {
      ESP_LOGW(kTag, "parsed 0 codes from %s", url.c_str());
    } else {
      ESP_LOGI(kTag, "fetched %u codes from %s",
               static_cast<unsigned>(out.size()), url.c_str());
    }
  } while (false);

  esp_http_client_cleanup(client);
  return out;
}

}  // namespace btclock
