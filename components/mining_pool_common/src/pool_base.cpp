#include "mining_pool_common/pool_base.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "tls_gate/tls_gate.hpp"

namespace btclock {
namespace mining_pools {
namespace {

constexpr const char* kTag = "pool.base";

// esp_http_client event handler that appends response-body bytes into
// a std::vector<char> accumulator. Stops appending once the cap is hit
// so a misbehaving server cannot balloon the heap.
struct FetchContext {
  std::vector<char> body;
  size_t cap = 0;
  bool truncated = false;
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchContext*>(evt->user_data);
  if (ctx == nullptr) return ESP_OK;
  if (evt->data == nullptr || evt->data_len <= 0) return ESP_OK;
  const size_t incoming = static_cast<size_t>(evt->data_len);
  if (ctx->body.size() + incoming > ctx->cap) {
    ctx->truncated = true;
    return ESP_OK;  // silently drop excess; we'll fail the parse
  }
  ctx->body.insert(ctx->body.end(),
                   static_cast<const char*>(evt->data),
                   static_cast<const char*>(evt->data) + incoming);
  return ESP_OK;
}

}  // namespace

PoolDataSource::PoolDataSource() = default;
PoolDataSource::~PoolDataSource() { Stop(); }

esp_err_t PoolDataSource::Start(DataHub& hub) {
  hub_ = &hub;
  stop_.store(false, std::memory_order_release);
  // 8 KB stack: cJSON parsing + esp_http_client + a few hundred bytes
  // of std::string churn fit comfortably. Mirror the old firmware's
  // 6 KB allocation plus headroom for the IDF HTTPS client.
  const BaseType_t ok = xTaskCreate(&PoolDataSource::TaskTrampoline,
                                    pool_name(), 8 * 1024, this,
                                    tskIDLE_PRIORITY + 1, &task_);
  return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t PoolDataSource::Stop() {
  stop_.store(true, std::memory_order_release);
  // The task checks stop_ before each sleep and exits via vTaskDelete
  // from within Run(), so we don't join here. Clear the hub pointer so
  // a stray Report() cannot fire post-Stop.
  hub_ = nullptr;
  task_ = nullptr;
  return ESP_OK;
}

void PoolDataSource::TaskTrampoline(void* arg) {
  static_cast<PoolDataSource*>(arg)->Run();
  vTaskDelete(nullptr);
}

void PoolDataSource::Run() {
  // First poll is staggered by 3 s to let Wi-Fi + time-sync settle
  // before we try the first handshake. The old firmware relied on the
  // minute-timer ISR, so the first sample could be up to 60 s out —
  // staggering brings that down without risking a handshake storm.
  vTaskDelay(pdMS_TO_TICKS(3000));
  while (!stop_.load(std::memory_order_acquire)) {
    PollOnce();
    // Sleep in 1 s chunks so Stop() is responsive.
    const uint32_t interval = poll_interval_ms();
    for (uint32_t elapsed = 0;
         elapsed < interval && !stop_.load(std::memory_order_acquire);
         elapsed += 1000) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

void PoolDataSource::PollOnce() {
  const std::string url = api_url();
  if (url.empty()) {
    ESP_LOGD(kTag, "%s: empty url, skipping", pool_name());
    return;
  }

  FetchContext ctx;
  ctx.cap = max_response_bytes();

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &http_event_handler;
  cfg.user_data = &ctx;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.buffer_size = 2048;       // rx header buffer
  cfg.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGW(kTag, "%s: http_client_init failed", pool_name());
    return;
  }

  const std::string token = auth_token();
  if (!token.empty()) {
    esp_http_client_set_header(client, "Pool-Auth-Token", token.c_str());
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  esp_err_t err;
  int status = 0;
  {
    // Hold the handshake gate only around perform — that's the window
    // in which mbedtls allocates its ~16 KB IN buffer and runs the TLS
    // handshake. Steady-state body reads, parsing, and reporting happen
    // with the gate released so other sources can handshake in parallel.
    std::lock_guard<std::mutex> lk(btclock::tls_gate::mutex());
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
  }

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "%s: perform failed: %s", pool_name(),
             esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "%s: HTTP %d", pool_name(), status);
    esp_http_client_cleanup(client);
    return;
  }
  if (ctx.truncated) {
    ESP_LOGW(kTag, "%s: response exceeded %u bytes, skipping parse",
             pool_name(), static_cast<unsigned>(ctx.cap));
    esp_http_client_cleanup(client);
    return;
  }
  esp_http_client_cleanup(client);

  // cJSON expects a NUL-terminated C string; accumulator is raw bytes.
  ctx.body.push_back('\0');

  ParsedStats parsed;
  parsed.name = pool_name();
  if (!parse_response(ctx.body.data(), parsed)) {
    ESP_LOGW(kTag, "%s: response parse failed (%u bytes)", pool_name(),
             static_cast<unsigned>(ctx.body.size() - 1));
    return;
  }
  if (parsed.hashrate.empty()) {
    // Keep the last good snapshot; don't blank it with "no sample".
    ESP_LOGD(kTag, "%s: parse ok but empty hashrate, skipping report",
             pool_name());
    return;
  }

  ESP_LOGI(kTag, "%s: hashrate=%s daily_sats=%lld", pool_name(),
           parsed.hashrate.c_str(),
           parsed.has_daily_sats
               ? static_cast<long long>(parsed.daily_sats)
               : 0LL);

  if (hub_ == nullptr) return;
  DataSnapshot partial;
  partial.pool.name = std::move(parsed.name);
  partial.pool.hashrate = std::move(parsed.hashrate);
  if (parsed.has_daily_sats) partial.pool.daily_sats = parsed.daily_sats;
  if (parsed.has_workers) partial.pool.workers = parsed.workers;
  hub_->Report(partial);
}

}  // namespace mining_pools
}  // namespace btclock
