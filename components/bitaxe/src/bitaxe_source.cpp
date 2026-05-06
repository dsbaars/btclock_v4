#include "bitaxe/bitaxe_source.hpp"

#include <cstring>
#include <utility>

#include "bitaxe/bitaxe_parser.hpp"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "prefs.hpp"
#include "proxy_transport/proxy_prefs.hpp"
#include "proxy_transport/proxy_transport.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace bitaxe {

namespace {
constexpr const char* kTag = "bitaxe";

// Response-body cap. AxeOS /api/system/info is ~1.5 KB currently;
// keep the cap tight — Rev A has only ~4 MB for OTA and mbedtls
// crowds the heap when every pool + price WS is active. 4 KB is
// 2× headroom with no risk of a runaway server eating DRAM.
constexpr std::size_t kMaxResponseBytes = 4 * 1024;

// Mirrors the PSRAM-first body accumulator in pool_base.cpp: lazy alloc
// on first byte, prefer SPIRAM with internal fallback, RAII cleanup.
// AxeOS responses are tiny (~1.5 KiB) so this is steady-state pressure
// relief rather than a hot-path concern.
struct FetchContext {
  char* body = nullptr;
  std::size_t size = 0;
  bool truncated = false;
  bool alloc_failed = false;

  ~FetchContext() {
    if (body) heap_caps_free(body);
  }
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchContext*>(evt->user_data);
  if (ctx == nullptr || evt->data == nullptr || evt->data_len <= 0) {
    return ESP_OK;
  }
  const std::size_t incoming = static_cast<std::size_t>(evt->data_len);
  if (ctx->size + incoming > kMaxResponseBytes) {
    ctx->truncated = true;
    return ESP_OK;
  }
  if (ctx->body == nullptr) {
    // +1 so the parser's trailing NUL always fits without a realloc.
    ctx->body = static_cast<char*>(heap_caps_malloc_prefer(
        kMaxResponseBytes + 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT));
    if (ctx->body == nullptr) {
      ctx->alloc_failed = true;
      return ESP_OK;
    }
  }
  std::memcpy(ctx->body + ctx->size, evt->data, incoming);
  ctx->size += incoming;
  return ESP_OK;
}
}  // namespace

BitaxeSource::BitaxeSource(std::string hostname)
    : hostname_(std::move(hostname)) {}

uint32_t BitaxeSource::poll_interval_ms() const {
  // Mirrors the schema bounds for kBitaxePollSec (5..300 s, default 10).
  // Read every tick so a live PATCH applies on the next poll.
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  uint32_t s = settings.GetU32(btclock::prefs::kBitaxePollSec, 10);
  if (s < 5) s = 5;
  if (s > 300) s = 300;
  return s * 1000;
}

BitaxeSource::~BitaxeSource() {
  Stop();
  if (done_ != nullptr) {
    vSemaphoreDelete(done_);
    done_ = nullptr;
  }
}

esp_err_t BitaxeSource::Start(DataHub& hub) {
  hub_ = &hub;
  stop_.store(false, std::memory_order_release);
  if (done_ == nullptr) {
    done_ = xSemaphoreCreateBinary();
    if (done_ == nullptr) {
      ESP_LOGE(kTag, "done semaphore alloc failed");
      hub_ = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }
  // 4 KB stack: cJSON + esp_http_client + the tiny response buffer all
  // fit. Lower priority than the WS data sources (tskIDLE_PRIORITY+1)
  // so a slow Bitaxe can't starve the main snapshot pipeline.
  const BaseType_t ok =
      xTaskCreate(&BitaxeSource::TaskTrampoline, "bitaxe", 4 * 1024, this,
                  tskIDLE_PRIORITY + 1, &task_);
  if (ok != pdPASS) {
    ESP_LOGE(kTag, "xTaskCreate failed; Bitaxe source disabled");
    task_ = nullptr;
    hub_ = nullptr;
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t BitaxeSource::Stop() {
  if (task_ == nullptr) {
    hub_ = nullptr;
    return ESP_OK;
  }
  stop_.store(true, std::memory_order_release);
  // Block until the poll task observes the flag and signals exit. The
  // run loop sleeps in 1 s chunks AND PollOnce holds the http client
  // for up to its connect/recv timeout (~5–10 s on a healthy LAN, can
  // spike during Wi-Fi degradation). 12 s covers the worst-case poll
  // already in flight; if the wait times out the OTA path proceeds
  // anyway because flash op locks already cover cache-disable races —
  // the wait just lets the task release its mbedtls / lwIP buffers
  // back to the heap before esp_https_ota_perform competes for them.
  if (done_ != nullptr) {
    if (xSemaphoreTake(done_, pdMS_TO_TICKS(12000)) != pdTRUE) {
      ESP_LOGW(kTag, "Stop() timed out waiting for task to exit");
    }
  }
  hub_ = nullptr;
  task_ = nullptr;
  return ESP_OK;
}

void BitaxeSource::TaskTrampoline(void* arg) {
  auto* self = static_cast<BitaxeSource*>(arg);
  self->Run();
  // Signal Stop()'s waiter BEFORE the self-delete; once vTaskDelete
  // runs, the task's stack is unwound and `self->done_` is the only
  // surviving handle into this object.
  if (self->done_ != nullptr) xSemaphoreGive(self->done_);
  vTaskDelete(nullptr);
}

void BitaxeSource::Run() {
  // Stagger the first poll so Wi-Fi + DNS settle before we hit the
  // Bitaxe's tiny HTTP server.
  vTaskDelay(pdMS_TO_TICKS(3000));
  while (!stop_.load(std::memory_order_acquire)) {
    PollOnce();
    // Re-read each tick so a live PATCH to `settings/bitaxePollSec`
    // applies on the next sleep without reboot.
    const uint32_t interval = poll_interval_ms();
    // Sleep in 1 s chunks so Stop() is responsive.
    for (uint32_t elapsed = 0;
         elapsed < interval && !stop_.load(std::memory_order_acquire);
         elapsed += 1000) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

void BitaxeSource::PollOnce() {
  if (hostname_.empty()) return;

  const std::string url = "http://" + hostname_ + "/api/system/info";

  FetchContext ctx;

  // Custom transport routes the connection through a SOCKS/HTTP proxy
  // when one is configured. With proxyEnabled=false the helper returns
  // a transport that does a direct TCP connect — same wire behaviour
  // as before this migration. AxeOS is plain HTTP on the LAN, so
  // ParamsForUrl picks use_tls=false here and the proxy stays in
  // bypass mode for any LAN destination via the default bypass globs.
  btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
  const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
  btclock::proxy::OwnedTransport proxy_t(btclock::proxy::MakeProxyTransport(
      proxy_cfg, btclock::proxy::ParamsForUrl(url)));

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &http_event_handler;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 5000;
  cfg.buffer_size = 1024;  // rx header buffer
  cfg.buffer_size_tx = 512;
  cfg.transport = proxy_t.get();

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGW(kTag, "http_client_init failed");
    return;
  }

  esp_http_client_set_header(client, "Accept", "application/json");

  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "perform failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "HTTP %d", status);
    esp_http_client_cleanup(client);
    return;
  }
  if (ctx.alloc_failed) {
    ESP_LOGW(kTag, "body alloc failed (cap=%u)",
             static_cast<unsigned>(kMaxResponseBytes));
    esp_http_client_cleanup(client);
    return;
  }
  if (ctx.truncated) {
    ESP_LOGW(kTag, "response exceeded %u bytes",
             static_cast<unsigned>(kMaxResponseBytes));
    esp_http_client_cleanup(client);
    return;
  }
  esp_http_client_cleanup(client);

  if (ctx.body == nullptr || ctx.size == 0) {
    ESP_LOGW(kTag, "empty body");
    return;
  }
  ctx.body[ctx.size] = '\0';

  ParsedStats parsed;
  if (!Parse(ctx.body, parsed)) {
    ESP_LOGW(kTag, "parse failed (%u bytes)", static_cast<unsigned>(ctx.size));
    return;
  }

  ESP_LOGI(kTag, "hashrate=%.1f GH/s diff=%s shares=%d",
           parsed.hashrate_ghs.value_or(0.0),
           parsed.best_diff ? parsed.best_diff->c_str() : "?",
           parsed.shares_accepted.value_or(0));

  if (hub_ == nullptr) return;
  DataSnapshot partial;
  partial.bitaxe.hostname = hostname_;
  partial.bitaxe.hashrate_ghs = parsed.hashrate_ghs;
  partial.bitaxe.best_diff = parsed.best_diff;
  partial.bitaxe.temperature_c = parsed.temperature_c;
  partial.bitaxe.shares_accepted = parsed.shares_accepted;
  hub_->Report(partial);
}

std::unique_ptr<DataSource> MakeBitaxeSource() {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  const bool enabled = settings.GetBool(btclock::prefs::kBitaxeEnabled, false);
  if (!enabled) {
    ESP_LOGI(kTag, "bitaxe disabled (settings/%s=false)",
             btclock::prefs::kBitaxeEnabled);
    return nullptr;
  }
  std::string host = settings.GetString(btclock::prefs::kBitaxeHostname, "");
  if (host.empty()) {
    ESP_LOGW(kTag, "bitaxe enabled but %s is empty; skipping",
             btclock::prefs::kBitaxeHostname);
    return nullptr;
  }
  ESP_LOGI(kTag, "bitaxe source on http://%s/api/system/info", host.c_str());
  return std::make_unique<BitaxeSource>(std::move(host));
}

}  // namespace bitaxe
}  // namespace btclock
