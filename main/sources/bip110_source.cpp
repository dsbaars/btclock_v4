#include "sources/bip110_source.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include "app/screen_slot_map.hpp"
#include "cJSON.h"
#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_transport_ws.h"
#include "net_util/user_agent.hpp"
#include "net_util/ws_client_lifecycle.hpp"
#include "prefs.hpp"
#include "proxy_transport/proxy_prefs.hpp"
#include "proxy_transport/proxy_transport.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "sources/mempool_parse.hpp"

namespace btclock {
namespace bip110 {

namespace {
constexpr const char* kTag = "bip110";
}  // namespace

Bip110Source::Bip110Source(std::string uri) : uri_(std::move(uri)) {}

Bip110Source::~Bip110Source() {
  Stop();
}

esp_err_t Bip110Source::Start(DataHub& hub) {
  hub_ = &hub;

  btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
  const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
  proxy_inner_ = btclock::proxy::MakeProxyTransport(
      proxy_cfg,
      btclock::proxy::ParamsForUrl(uri_.c_str(), esp_crt_bundle_attach));
  proxy_ws_ = esp_transport_ws_init(proxy_inner_);
  if (proxy_ws_ == nullptr) {
    if (proxy_inner_ != nullptr) {
      esp_transport_destroy(proxy_inner_);
      proxy_inner_ = nullptr;
    }
    ESP_LOGE(kTag, "proxy ws init failed");
    hub_ = nullptr;
    return ESP_FAIL;
  }
  esp_transport_set_default_port(
      proxy_ws_, btclock::proxy::UrlImpliesTls(uri_.c_str()) ? 443 : 80);

  const std::string ws_path = btclock::proxy::PathFromUri(uri_.c_str());
  esp_transport_ws_config_t ws_cfg = {};
  ws_cfg.ws_path = ws_path.c_str();
  ws_cfg.propagate_control_frames = true;
  esp_transport_ws_set_config(proxy_ws_, &ws_cfg);

  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri_.c_str();
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 30;
  cfg.pingpong_timeout_sec = 20;
  // Deliberately much smaller than the mempool+kraken leg's 16 KB. That
  // source oversizes its buffer so the snapshot lands in a single event;
  // here the snapshot is a once-per-connect cost on a secondary screen,
  // so we take the fragment reassembly in HandleEvent instead and keep
  // the steady-state footprint of a third WSS client down. Post-snapshot
  // `block` pushes fit in one 4 KB event anyway.
  cfg.buffer_size = 4096;
  // cJSON recurses through the block objects' nested `extras`; 6 KB
  // matches what the mempool leg runs with.
  cfg.task_stack = 6144;
  cfg.user_agent = btclock::net_util::WebsocketUserAgent();
  cfg.ext_transport = proxy_ws_;

  client_ = esp_websocket_client_init(&cfg);
  if (client_ == nullptr) {
    ESP_LOGE(kTag, "client init failed");
    esp_transport_destroy(proxy_ws_);
    proxy_ws_ = nullptr;
    esp_transport_destroy(proxy_inner_);
    proxy_inner_ = nullptr;
    hub_ = nullptr;
    return ESP_FAIL;
  }
  if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                    &Bip110Source::EventTrampoline,
                                    this) != ESP_OK) {
    ESP_LOGE(kTag, "register_events failed");
  }
  ESP_LOGW(kTag, "connecting: %s", uri_.c_str());
  const esp_err_t rc = esp_websocket_client_start(client_);
  if (rc != ESP_OK) {
    // Don't fail Start() — the built-in retry loop reconnects once the
    // network recovers, and a dead secondary source must not take the
    // hub's StartAll aggregate down with it.
    ESP_LOGW(kTag, "start: %s", esp_err_to_name(rc));
  }
  return ESP_OK;
}

esp_err_t Bip110Source::Stop() {
  if (client_ != nullptr) {
    // Null first, shutdown second — same lifecycle-vs-wire-state race
    // the other WS sources guard against.
    auto* to_destroy = client_;
    client_ = nullptr;
    SafeShutdownWsClient(to_destroy);
  }
  if (proxy_ws_ != nullptr) {
    esp_transport_destroy(proxy_ws_);
    proxy_ws_ = nullptr;
  }
  if (proxy_inner_ != nullptr) {
    esp_transport_destroy(proxy_inner_);
    proxy_inner_ = nullptr;
  }
  connected_.store(false);
  // Release the snapshot-sized accumulator rather than carrying it as
  // idle capacity for the life of the object.
  std::string().swap(frame_);
  frame_overflow_ = false;
  hub_ = nullptr;
  return ESP_OK;
}

void Bip110Source::EventTrampoline(void* arg, esp_event_base_t,
                                   int32_t event_id, void* event_data) {
  static_cast<Bip110Source*>(arg)->HandleEvent(event_id, event_data);
}

void Bip110Source::HandleEvent(int32_t event_id, void* event_data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      connected_.store(true);
      ESP_LOGW(kTag, "ws connected");
      frame_.clear();
      frame_overflow_ = false;
      SendSubscription();
      // No hub_->ForceNotify() here, unlike the primary sources: this is
      // a secondary feed, and the snapshot frame that follows this
      // subscribe carries the tip anyway. A forced notify would only add
      // a redundant render pass on every reconnect.
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      connected_.store(false);
      ESP_LOGW(kTag, "ws disconnected");
      std::string().swap(frame_);
      frame_overflow_ = false;
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 0x1 = text, 0x0 = continuation. The `blocks` snapshot is
      // ~17 KB against a 4 KB rx buffer, so reassembly is the normal
      // path here rather than an edge case.
      if (ev != nullptr && (ev->op_code == 0x1 || ev->op_code == 0x0) &&
          ev->data_len > 0) {
        if (ev->payload_offset == 0) {
          frame_.clear();
          frame_overflow_ = false;
        }
        if (!frame_overflow_) {
          if (frame_.size() + static_cast<std::size_t>(ev->data_len) >
              kMaxFrameBytes) {
            frame_overflow_ = true;
            std::string().swap(frame_);
            ESP_LOGW(kTag, "frame exceeded %u bytes; dropping",
                     static_cast<unsigned>(kMaxFrameBytes));
          } else {
            frame_.append(ev->data_ptr, ev->data_len);
          }
        }
        const int total = ev->payload_offset + ev->data_len;
        if (total >= ev->payload_len) {
          if (!frame_overflow_ && !frame_.empty()) {
            HandleFrame(frame_.data(), frame_.size());
          }
          std::string().swap(frame_);
          frame_overflow_ = false;
        }
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      connected_.store(false);
      ESP_LOGW(kTag, "ws error");
      break;
    default:
      break;
  }
}

void Bip110Source::SendSubscription() {
  // Only `blocks` — the screen shows heights, and subscribing to fees /
  // mempool-blocks would add steady-state traffic nothing reads.
  static constexpr const char* kSub =
      "{\"action\":\"want\",\"data\":[\"blocks\"]}";
  const int n = esp_websocket_client_send_text(client_, kSub, std::strlen(kSub),
                                               pdMS_TO_TICKS(2000));
  if (n < 0) {
    ESP_LOGW(kTag, "subscribe send failed");
  }
}

void Bip110Source::HandleFrame(const char* data, std::size_t len) {
  cJSON* root = cJSON_ParseWithLength(data, len);
  if (root == nullptr) {
    ESP_LOGW(kTag, "json parse failed (%u bytes)", static_cast<unsigned>(len));
    return;
  }

  uint32_t height = 0;
  bool have = false;

  // `block` (singular) — one newly confirmed block, the steady-state
  // push. `blocks` (plural) — the ascending initial snapshot; shared
  // helper picks the tip out of it so both chains resolve their tip
  // through identical code.
  const cJSON* block = cJSON_GetObjectItemCaseSensitive(root, "block");
  if (cJSON_IsObject(block)) {
    const cJSON* h = cJSON_GetObjectItemCaseSensitive(block, "height");
    if (cJSON_IsNumber(h) && h->valuedouble > 0) {
      height = static_cast<uint32_t>(h->valuedouble);
      have = true;
    }
  }
  if (!have) {
    const cJSON* blocks = cJSON_GetObjectItemCaseSensitive(root, "blocks");
    have = TipHeightFromBlocksArray(blocks, &height);
  }
  cJSON_Delete(root);

  // 0 is the renderer's "no sample yet" sentinel, so it must never reach
  // the snapshot as a real reading.
  if (!have || height == 0) return;

  ESP_LOGW(kTag, "tip=%u", static_cast<unsigned>(height));

  if (hub_ == nullptr) return;
  DataSnapshot partial;
  partial.bip110_block_height = height;
  hub_->Report(partial);
}

std::unique_ptr<DataSource> MakeBip110Source() {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);

  // Nothing on the device reads bip110_block_height except the dual
  // block-height screen, so a hidden screen means holding a WSS
  // connection open against a third-party host for nothing. Matches the
  // visibility key the WebUI picker and control_command_drain.cpp write.
  char vkey[24];
  std::snprintf(vkey, sizeof(vkey), "screen%dVisible",
                slot_map::kApiIdBlockHeightSplit);
  if (!settings.GetBool(vkey, slot_map::DefaultScreenVisible(
                                  slot_map::kApiIdBlockHeightSplit))) {
    ESP_LOGW(kTag, "screen hidden; BIP-110 source not started");
    return nullptr;
  }

  std::string uri =
      settings.GetString(btclock::prefs::kBip110Endpoint, kDefaultUri);
  if (uri.empty()) uri = kDefaultUri;
  return std::make_unique<Bip110Source>(std::move(uri));
}

}  // namespace bip110
}  // namespace btclock
