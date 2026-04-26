#include "nostr/relay_client.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace btclock {
namespace nostr {
namespace {
constexpr const char* kTag = "nostr-relay";
}  // namespace

RelayClient::RelayClient(std::string url) : url_(std::move(url)) {}

RelayClient::~RelayClient() {
  Stop();
}

esp_err_t RelayClient::Start() {
  if (client_ != nullptr) return ESP_OK;

  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 30;  // relays often pong every ~30 s
  cfg.pingpong_timeout_sec = 20;
  cfg.buffer_size = 8192;  // zap receipts can carry large description tags
  cfg.task_stack = 6144;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  client_ = esp_websocket_client_init(&cfg);
  if (client_ == nullptr) return ESP_FAIL;

  ESP_RETURN_ON_FALSE(
      esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                    &EventTrampoline, this) == ESP_OK,
      ESP_FAIL, kTag, "register_events");

  ESP_LOGI(kTag, "connecting: %s", url_.c_str());
  return esp_websocket_client_start(client_);
}

esp_err_t RelayClient::Stop() {
  if (client_ == nullptr) return ESP_OK;
  esp_websocket_client_stop(client_);
  esp_websocket_client_destroy(client_);
  client_ = nullptr;
  connected_ = false;
  return ESP_OK;
}

bool RelayClient::SendText(const char* data, size_t len) {
  if (client_ == nullptr || !connected_) return false;
  const int n = esp_websocket_client_send_text(
      client_, data, static_cast<int>(len), pdMS_TO_TICKS(2000));
  return n >= 0;
}

void RelayClient::EventTrampoline(void* arg, esp_event_base_t, int32_t id,
                                  void* data) {
  static_cast<RelayClient*>(arg)->HandleEvent(id, data);
}

void RelayClient::HandleEvent(int32_t id, void* data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(data);
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      connected_ = true;
      ESP_LOGI(kTag, "connected: %s", url_.c_str());
      if (on_connect_) on_connect_(true);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      connected_ = false;
      ESP_LOGW(kTag, "disconnected: %s", url_.c_str());
      if (on_connect_) on_connect_(false);
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 0x1 = text frame (NIP-01 is UTF-8 text).
      if (ev && ev->op_code == 0x1 && on_frame_) {
        on_frame_(ev->data_ptr, static_cast<size_t>(ev->data_len));
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(kTag, "error: %s", url_.c_str());
      break;
    default:
      break;
  }
}

}  // namespace nostr
}  // namespace btclock
