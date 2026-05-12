#include "nostr/relay_client.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport_ws.h"
#include "proxy_transport/proxy_prefs.hpp"
#include "proxy_transport/proxy_transport.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

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

  // Build the proxy-aware ext_transport. Mirrors the pattern used by
  // BtclockDataSource — both inner + ws handles are class members so
  // Stop() can destroy them after the WS client.
  {
    btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
    const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
    proxy_inner_ = btclock::proxy::MakeProxyTransport(
        proxy_cfg, btclock::proxy::ParamsForUrl(url_, esp_crt_bundle_attach));
    proxy_ws_ = esp_transport_ws_init(proxy_inner_);
    if (proxy_ws_) {
      esp_transport_set_default_port(
          proxy_ws_, btclock::proxy::UrlImpliesTls(url_) ? 443 : 80);
    }
  }
  if (!proxy_ws_) {
    if (proxy_inner_) {
      esp_transport_destroy(proxy_inner_);
      proxy_inner_ = nullptr;
    }
    return ESP_FAIL;
  }
  // ext_transport bypasses the WS client's internal apply
  // (esp_websocket_client.c:582,647), so push the path explicitly.
  const std::string ws_path = btclock::proxy::PathFromUri(url_);
  esp_transport_ws_config_t ws_cfg = {};
  ws_cfg.ws_path = ws_path.c_str();
  ws_cfg.propagate_control_frames = true;
  esp_transport_ws_set_config(proxy_ws_, &ws_cfg);

  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 30;  // relays often pong every ~30 s
  cfg.pingpong_timeout_sec = 20;
  cfg.buffer_size = 8192;  // zap receipts can carry large description tags
  // secp256k1 schnorr verification runs inline on this task (via the
  // on_frame_ → SubscriptionManager → VerifyEvent path), which needs
  // ~3 KB of scratch on top of the WebSocket internal overhead and
  // Sha256Ctx frame. 12 KB is the measured-safe threshold; 6 KB caused
  // a hard stack-overflow boot-loop in production.
  cfg.task_stack = 12288;
  cfg.ext_transport = proxy_ws_;

  client_ = esp_websocket_client_init(&cfg);
  if (client_ == nullptr) {
    esp_transport_destroy(proxy_ws_);
    proxy_ws_ = nullptr;
    esp_transport_destroy(proxy_inner_);
    proxy_inner_ = nullptr;
    return ESP_FAIL;
  }

  ESP_RETURN_ON_FALSE(
      esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                    &EventTrampoline, this) == ESP_OK,
      ESP_FAIL, kTag, "register_events");

  ESP_LOGI(kTag, "connecting: %s", url_.c_str());
  return esp_websocket_client_start(client_);
}

esp_err_t RelayClient::Stop() {
  if (client_ == nullptr) {
    if (proxy_ws_) {
      esp_transport_destroy(proxy_ws_);
      proxy_ws_ = nullptr;
    }
    if (proxy_inner_) {
      esp_transport_destroy(proxy_inner_);
      proxy_inner_ = nullptr;
    }
    return ESP_OK;
  }
  esp_websocket_client_stop(client_);
  esp_websocket_client_destroy(client_);
  client_ = nullptr;
  if (proxy_ws_) {
    esp_transport_destroy(proxy_ws_);
    proxy_ws_ = nullptr;
  }
  if (proxy_inner_) {
    esp_transport_destroy(proxy_inner_);
    proxy_inner_ = nullptr;
  }
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
    case WEBSOCKET_EVENT_CONNECTED: {
      const int64_t now_ms = esp_timer_get_time() / 1000;
      const bool was_connected_before = last_connect_ms_.load() != 0;
      connected_ = true;
      last_connect_ms_.store(now_ms);
      if (was_connected_before) reconnect_count_.fetch_add(1);
      ESP_LOGI(kTag, "connected: %s", url_.c_str());
      if (on_connect_) on_connect_(true);
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
      connected_ = false;
      last_disconnect_ms_.store(esp_timer_get_time() / 1000);
      // A mid-frame disconnect leaves the accumulator holding a
      // partial event — discard so the next reconnect doesn't
      // prepend stale bytes to a fresh frame.
      fragment_buf_.clear();
      ESP_LOGW(kTag, "disconnected: %s", url_.c_str());
      if (on_connect_) on_connect_(false);
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 0x1 = TEXT initial fragment, 0x0 = CONTINUATION.
      // esp_websocket_client fires multiple WEBSOCKET_EVENT_DATA per
      // logical WS frame when the payload exceeds the internal TCP
      // segment buffer — every kind 23197/23196 NWC payment
      // notification and every long zap receipt (NIP-57 kind 9735
      // with `description` tag) is large enough to fragment, while
      // INFO (13194) and get_balance (23195) responses fit in one
      // chunk. Reassemble using payload_offset / payload_len before
      // forwarding to on_frame_, otherwise ParseEnvelope sees a
      // truncated JSON and silently drops the event. Same pattern as
      // mempool_kraken_source.cpp.
      if (ev && (ev->op_code == 0x1 || ev->op_code == 0x0) &&
          ev->data_len > 0) {
        frames_chunk_.fetch_add(1);
        if (ev->payload_offset == 0) fragment_buf_.clear();
        fragment_buf_.append(ev->data_ptr, ev->data_len);
        const int total = ev->payload_offset + ev->data_len;
        if (total >= ev->payload_len) {
          last_frame_bytes_.store(
              static_cast<uint32_t>(fragment_buf_.size()));
          frames_complete_.fetch_add(1);
          if (on_frame_) on_frame_(fragment_buf_.data(), fragment_buf_.size());
          fragment_buf_.clear();
        }
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
