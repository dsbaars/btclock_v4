#include "mining_pool_common/pool_base.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "tls_gate/tls_gate.hpp"

namespace btclock {
namespace mining_pools {
namespace {

constexpr const char* kTag = "pool.base";

// Response-body accumulator backed by PSRAM. Pool responses can reach
// 32–64 KiB (public_pool worker list) — previously a std::vector<char>
// did this on internal heap and crowded the ~32 KiB of steady-state
// free DRAM. PSRAM has 1.9 MiB free on Rev B / 7 MiB on V8, so parking
// these cold buffers there is essentially free. The buffer is only
// walked once (by parse_response) after the HTTP transfer completes,
// so the PSRAM access latency is irrelevant.
struct FetchContext {
  char* body = nullptr;
  size_t size = 0;
  size_t cap = 0;
  bool truncated = false;
  bool alloc_failed = false;

  ~FetchContext() {
    if (body) heap_caps_free(body);
  }
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchContext*>(evt->user_data);
  if (ctx == nullptr) return ESP_OK;
  if (evt->data == nullptr || evt->data_len <= 0) return ESP_OK;
  const size_t incoming = static_cast<size_t>(evt->data_len);
  if (ctx->size + incoming > ctx->cap) {
    ctx->truncated = true;
    return ESP_OK;  // silently drop excess; we'll fail the parse
  }
  // Lazy-allocate on first byte so empty responses don't touch heap at
  // all. Prefer SPIRAM, fall back to internal — heap_caps_malloc_prefer
  // handles both caps in one call.
  if (ctx->body == nullptr) {
    // +1 so the parser's trailing NUL always fits without a realloc.
    ctx->body = static_cast<char*>(heap_caps_malloc_prefer(
        ctx->cap + 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT));
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

PoolDataSource::PoolDataSource() = default;
PoolDataSource::~PoolDataSource() {
  Stop();
  if (done_ != nullptr) {
    vSemaphoreDelete(done_);
    done_ = nullptr;
  }
}

uint32_t PoolDataSource::poll_interval_ms() const {
  // Mirrors the schema bounds for kPoolPollSec (10..3600 s, default 60).
  // Read every tick so a live PATCH applies on the next poll.
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  uint32_t s = settings.GetU32(btclock::prefs::kPoolPollSec, 60);
  if (s < 10) s = 10;
  if (s > 3600) s = 3600;
  return s * 1000;
}

esp_err_t PoolDataSource::Start(DataHub& hub) {
  hub_ = &hub;
  stop_.store(false, std::memory_order_release);
  if (done_ == nullptr) {
    done_ = xSemaphoreCreateBinary();
    if (done_ == nullptr) {
      ESP_LOGE(kTag, "%s: done semaphore alloc failed", pool_name());
      hub_ = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }
  // 8 KB stack: cJSON parsing + esp_http_client + a few hundred bytes
  // of std::string churn fit comfortably. Mirror the old firmware's
  // 6 KB allocation plus headroom for the IDF HTTPS client.
  const BaseType_t ok =
      xTaskCreate(&PoolDataSource::TaskTrampoline, pool_name(), 8 * 1024, this,
                  tskIDLE_PRIORITY + 1, &task_);
  if (ok != pdPASS) {
    ESP_LOGE(kTag, "%s: xTaskCreate failed; pool source disabled", pool_name());
    task_ = nullptr;
    hub_ = nullptr;
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t PoolDataSource::Stop() {
  if (task_ == nullptr) {
    hub_ = nullptr;
    return ESP_OK;
  }
  stop_.store(true, std::memory_order_release);
  // Block until the poll task observes the flag and signals exit. Pool
  // pollers do an HTTPS GET (TLS handshake + JSON body), which can take
  // a few seconds under normal conditions and longer under network
  // degradation. 12 s covers the typical worst-case poll already in
  // flight; if the wait times out the OTA path proceeds anyway because
  // flash op locks already cover cache-disable races — the wait just
  // lets the task release its mbedtls / lwIP buffers back to the heap
  // before esp_https_ota_perform competes for them.
  if (done_ != nullptr) {
    if (xSemaphoreTake(done_, pdMS_TO_TICKS(12000)) != pdTRUE) {
      ESP_LOGW(kTag, "%s: Stop() timed out waiting for task to exit",
               pool_name());
    }
  }
  hub_ = nullptr;
  task_ = nullptr;
  return ESP_OK;
}

void PoolDataSource::TaskTrampoline(void* arg) {
  auto* self = static_cast<PoolDataSource*>(arg);
  self->Run();
  // Signal Stop()'s waiter BEFORE the self-delete; once vTaskDelete
  // runs, the task's stack is unwound and `self->done_` is the only
  // surviving handle into this object.
  if (self->done_ != nullptr) xSemaphoreGive(self->done_);
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
  cfg.buffer_size = 2048;  // rx header buffer
  cfg.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGW(kTag, "%s: http_client_init failed", pool_name());
    return;
  }

  const std::string token = auth_token();
  if (!token.empty()) {
    esp_http_client_set_header(client, auth_header_name(), token.c_str());
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
    ESP_LOGW(kTag, "%s: perform failed: %s", pool_name(), esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "%s: HTTP %d", pool_name(), status);
    esp_http_client_cleanup(client);
    return;
  }
  if (ctx.alloc_failed) {
    ESP_LOGW(kTag, "%s: body alloc failed (cap=%u)", pool_name(),
             static_cast<unsigned>(ctx.cap));
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

  if (ctx.body == nullptr) {
    ESP_LOGW(kTag, "%s: empty response body", pool_name());
    return;
  }
  // cJSON expects a NUL-terminated C string; accumulator is raw bytes.
  ctx.body[ctx.size] = '\0';

  ParsedStats parsed;
  parsed.name = pool_name();
  if (!parse_response(ctx.body, parsed)) {
    ESP_LOGW(kTag, "%s: response parse failed (%u bytes)", pool_name(),
             static_cast<unsigned>(ctx.size));
    return;
  }
  if (parsed.hashrate.empty()) {
    // Keep the last good snapshot; don't blank it with "no sample".
    ESP_LOGD(kTag, "%s: parse ok but empty hashrate, skipping report",
             pool_name());
    return;
  }

  ESP_LOGI(
      kTag, "%s: hashrate=%s daily_sats=%lld", pool_name(),
      parsed.hashrate.c_str(),
      parsed.has_daily_sats ? static_cast<long long>(parsed.daily_sats) : 0LL);

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
