#include "bitaxe/bitaxe_source.hpp"

#include <utility>
#include <vector>

#include "bitaxe/bitaxe_parser.hpp"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_http_client.h"
#include "esp_log.h"
#include "prefs.hpp"
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

struct FetchContext {
  std::vector<char> body;
  bool truncated = false;
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchContext*>(evt->user_data);
  if (ctx == nullptr || evt->data == nullptr || evt->data_len <= 0) {
    return ESP_OK;
  }
  const std::size_t incoming = static_cast<std::size_t>(evt->data_len);
  if (ctx->body.size() + incoming > kMaxResponseBytes) {
    ctx->truncated = true;
    return ESP_OK;
  }
  ctx->body.insert(ctx->body.end(),
                   static_cast<const char*>(evt->data),
                   static_cast<const char*>(evt->data) + incoming);
  return ESP_OK;
}
}  // namespace

BitaxeSource::BitaxeSource(std::string hostname,
                            uint32_t poll_interval_ms)
    : hostname_(std::move(hostname)),
      poll_interval_ms_(poll_interval_ms) {}

BitaxeSource::~BitaxeSource() { Stop(); }

esp_err_t BitaxeSource::Start(DataHub& hub) {
  hub_ = &hub;
  stop_.store(false, std::memory_order_release);
  // 4 KB stack: cJSON + esp_http_client + the tiny response buffer all
  // fit. Lower priority than the WS data sources (tskIDLE_PRIORITY+1)
  // so a slow Bitaxe can't starve the main snapshot pipeline.
  const BaseType_t ok = xTaskCreate(&BitaxeSource::TaskTrampoline,
                                     "bitaxe", 4 * 1024, this,
                                     tskIDLE_PRIORITY + 1, &task_);
  return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t BitaxeSource::Stop() {
  stop_.store(true, std::memory_order_release);
  // Task self-deletes via vTaskDelete(nullptr) when it observes the
  // stop flag, so we don't join here. Clear hub so a late-arriving
  // Report() cannot fire post-Stop.
  hub_ = nullptr;
  task_ = nullptr;
  return ESP_OK;
}

void BitaxeSource::TaskTrampoline(void* arg) {
  static_cast<BitaxeSource*>(arg)->Run();
  vTaskDelete(nullptr);
}

void BitaxeSource::Run() {
  // Stagger the first poll so Wi-Fi + DNS settle before we hit the
  // Bitaxe's tiny HTTP server.
  vTaskDelay(pdMS_TO_TICKS(3000));
  while (!stop_.load(std::memory_order_acquire)) {
    PollOnce();
    const uint32_t interval = poll_interval_ms_;
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

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &http_event_handler;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 5000;
  cfg.buffer_size = 1024;       // rx header buffer
  cfg.buffer_size_tx = 512;

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
  if (ctx.truncated) {
    ESP_LOGW(kTag, "response exceeded %u bytes",
             static_cast<unsigned>(kMaxResponseBytes));
    esp_http_client_cleanup(client);
    return;
  }
  esp_http_client_cleanup(client);

  ctx.body.push_back('\0');

  ParsedStats parsed;
  if (!Parse(ctx.body.data(), parsed)) {
    ESP_LOGW(kTag, "parse failed (%u bytes)",
             static_cast<unsigned>(ctx.body.size() - 1));
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
  const bool enabled =
      settings.GetBool(btclock::prefs::kBitaxeEnabled, false);
  if (!enabled) {
    ESP_LOGI(kTag, "bitaxe disabled (settings/%s=false)",
             btclock::prefs::kBitaxeEnabled);
    return nullptr;
  }
  std::string host =
      settings.GetString(btclock::prefs::kBitaxeHostname, "");
  if (host.empty()) {
    ESP_LOGW(kTag, "bitaxe enabled but %s is empty; skipping",
             btclock::prefs::kBitaxeHostname);
    return nullptr;
  }
  ESP_LOGI(kTag, "bitaxe source on http://%s/api/system/info",
           host.c_str());
  return std::make_unique<BitaxeSource>(std::move(host));
}

}  // namespace bitaxe
}  // namespace btclock
