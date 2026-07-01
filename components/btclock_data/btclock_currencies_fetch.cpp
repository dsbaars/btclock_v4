#include "btclock_currencies_fetch.hpp"

#include <cstring>
#include <vector>

#include "btclock_currencies_parse.hpp"
#include "btclock_currencies_uri.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "proxy_transport/proxy_prefs.hpp"
#include "proxy_transport/proxy_transport.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock-curr";

// Budget for the JSON array body. The upstream catalogue has grown to
// ~165 codes (~1 KiB and climbing); the old 1 KiB cap sat at ~97%
// utilisation, so any further growth tipped it over and HttpEvent set
// `truncated`, which discarded the whole list back to the seeded set.
// 8 KiB gives years of headroom while still bounding a captive-portal
// HTML error page (which ParseCurrenciesJson rejects anyway).
constexpr std::size_t kMaxBodyBytes = 8192;

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
  btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
  const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
  btclock::proxy::OwnedTransport proxy_t(btclock::proxy::MakeProxyTransport(
      proxy_cfg, btclock::proxy::ParamsForUrl(url, esp_crt_bundle_attach)));

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &HttpEvent;
  cfg.user_data = &ctx;
  // Per-attempt timeout, kept modest so the retry loop below can't stall
  // the main task (this runs on the event-loop tick at first STA connect)
  // for long on a dead network.
  cfg.timeout_ms = 6000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport = proxy_t.get();

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGW(kTag, "esp_http_client_init failed for %s", url.c_str());
    return {};
  }

  // The fetch is a one-shot on the first STA connect (network_coordinator),
  // so a transient TLS/timeout right at GOT_IP would otherwise strand the
  // device on the seeded catalogue until the next reboot. Retry a few times
  // with a short backoff; on a healthy link the first attempt succeeds in
  // well under a second.
  constexpr int kAttempts = 3;
  std::vector<std::string> out;
  for (int attempt = 0; attempt < kAttempts && out.empty(); ++attempt) {
    if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(500));
    ctx.buf.clear();
    ctx.truncated = false;
    if (esp_http_client_perform(client) != ESP_OK) {
      ESP_LOGW(kTag, "perform failed for %s (attempt %d/%d)", url.c_str(),
               attempt + 1, kAttempts);
      continue;
    }
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
      ESP_LOGW(kTag, "non-2xx (%d) from %s (attempt %d/%d)", status,
               url.c_str(), attempt + 1, kAttempts);
      continue;
    }
    if (ctx.truncated || ctx.buf.empty()) {
      ESP_LOGW(kTag, "empty/truncated body from %s (truncated=%d size=%u)",
               url.c_str(), ctx.truncated ? 1 : 0,
               static_cast<unsigned>(ctx.buf.size()));
      continue;
    }
    out = ParseCurrenciesJson(ctx.buf.data(), ctx.buf.size());
    if (out.empty()) {
      ESP_LOGW(kTag, "parsed 0 codes from %s", url.c_str());
    } else {
      ESP_LOGI(kTag, "fetched %u codes from %s (attempt %d)",
               static_cast<unsigned>(out.size()), url.c_str(), attempt + 1);
    }
  }

  esp_http_client_cleanup(client);
  return out;
}

}  // namespace btclock
