#include "btclock_data.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "ArduinoJson.h"
#include "btclock_subscribe.hpp"
#include "data_core/hub.hpp"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock-data";
}  // namespace

BtclockDataSource::BtclockDataSource(const char* uri,
                                     std::vector<std::string> currencies,
                                     bool block_fee_dec)
    : uri_(uri),
      currencies_(std::move(currencies)),
      block_fee_dec_(block_fee_dec) {}

BtclockDataSource::~BtclockDataSource() {
  Stop();
}

esp_err_t BtclockDataSource::Start(DataHub& hub) {
  hub_ = &hub;

  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri_.c_str();
  cfg.reconnect_timeout_ms = 5000;  // linear backoff, 5 s between retries
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 20;
  cfg.pingpong_timeout_sec = 15;
  cfg.buffer_size = 4096;  // headroom for price map frames
  cfg.task_stack = 6144;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  client_ = esp_websocket_client_init(&cfg);
  if (client_ == nullptr) return ESP_FAIL;

  ESP_RETURN_ON_FALSE(
      esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                    &EventHandlerTrampoline, this) == ESP_OK,
      ESP_FAIL, kTag, "register_events");

  // Mirror the nostr-relay log shape ("connecting: %s") so a user
  // grepping the serial console can spot both ws clients with the same
  // pattern.
  ESP_LOGI(kTag, "connecting: %s", uri_.c_str());
  return esp_websocket_client_start(client_);
}

void BtclockDataSource::SetCurrencies(std::vector<std::string> currencies) {
  if (currencies.empty()) return;
  currencies_ = std::move(currencies);
  // Bounce the WS so the new subscription set lands on a clean session.
  // Only do the restart when we were actually running — pre-Start calls
  // (e.g. test harnesses) just update the field and the next Start picks
  // it up.
  if (client_ != nullptr && hub_ != nullptr) {
    DataHub* h = hub_;
    Stop();
    Start(*h);
  }
}

void BtclockDataSource::SetBlockFeeDec(bool block_fee_dec) {
  if (block_fee_dec_ == block_fee_dec) return;
  block_fee_dec_ = block_fee_dec;
  // Same Stop+Start pattern as SetCurrencies: an additive `subscribe`
  // alone wouldn't tell the relay to drop the previously-subscribed
  // fee stream, so we'd still see double-dispatch until a reconnect.
  if (client_ != nullptr && hub_ != nullptr) {
    DataHub* h = hub_;
    Stop();
    Start(*h);
  }
}

esp_err_t BtclockDataSource::Stop() {
  if (client_ == nullptr) {
    hub_ = nullptr;
    return ESP_OK;
  }
  esp_websocket_client_stop(client_);
  esp_websocket_client_destroy(client_);
  client_ = nullptr;
  hub_ = nullptr;
  return ESP_OK;
}

void BtclockDataSource::EventHandlerTrampoline(void* arg, esp_event_base_t,
                                               int32_t event_id,
                                               void* event_data) {
  static_cast<BtclockDataSource*>(arg)->HandleEvent(event_id, event_data);
}

void BtclockDataSource::HandleEvent(int32_t event_id, void* event_data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(kTag, "ws connected");
      SendSubscriptions();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(kTag, "ws disconnected");
      break;
    case WEBSOCKET_EVENT_DATA:
      if (ev && ev->op_code == 0x2 /* binary */) {
        HandleBinaryFrame(reinterpret_cast<const uint8_t*>(ev->data_ptr),
                          ev->data_len);
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(kTag, "ws error");
      break;
    default:
      break;
  }
}

void BtclockDataSource::SendSubscriptions() {
  // Three small MessagePack-encoded subscribe frames. Encode each into a
  // stack buffer and send as a binary frame.
  auto send_sub = [&](const char* event_type, const char* currency) {
    JsonDocument doc;
    doc["type"] = "subscribe";
    doc["eventType"] = event_type;
    if (currency != nullptr) doc["currency"] = currency;

    uint8_t buf[96];
    const size_t n = serializeMsgPack(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
      ESP_LOGE(kTag, "msgpack encode failed for %s", event_type);
      return;
    }
    const int sent = esp_websocket_client_send_bin(
        client_, reinterpret_cast<const char*>(buf), n, pdMS_TO_TICKS(2000));
    if (sent < 0) ESP_LOGW(kTag, "send_bin failed (%s)", event_type);
  };

  send_sub("blockheight", nullptr);
  // Subscribe to exactly one fee stream — picked by the `blockFeeDec`
  // pref. Both topics still exist on the relay; subscribing to both
  // (the previous behaviour) caused HandleBinaryFrame to write the
  // snapshot's fee field twice per tick from two different precisions,
  // and any fee-screen renderer that read the wrong field would lag a
  // wire-frame behind. `blockfee2` is the 2-decimal stream and fires
  // every fee tick; `blockfee` is the integer-rounded stream.
  send_sub(block_fee_dec_ ? "blockfee2" : "blockfee", nullptr);
  for (const auto& ccy : currencies_) {
    send_sub("price", ccy.c_str());
  }
  ESP_LOGI(
      kTag, "%s",
      subscribe::BuildSubscribeLogLine(currencies_, block_fee_dec_).c_str());
}

void BtclockDataSource::HandleBinaryFrame(const uint8_t* data, size_t len) {
  JsonDocument doc;
  const DeserializationError err = deserializeMsgPack(doc, data, len);
  if (err) {
    ESP_LOGW(kTag, "msgpack decode failed: %s (%d bytes)", err.c_str(),
             static_cast<int>(len));
    return;
  }

  // Ack / status frames: {"msg": "..."} or {"error": "..."}. Log and skip.
  if (doc["msg"].is<const char*>()) {
    ESP_LOGI(kTag, "srv: %s", doc["msg"].as<const char*>());
  }
  if (doc["error"].is<const char*>()) {
    ESP_LOGW(kTag, "srv error: %s", doc["error"].as<const char*>());
  }

  // Build a partial snapshot — only the fields present in this frame.
  // Hub ignores all-empty partials, so this is cheap.
  DataSnapshot partial;

  if (doc["blockheight"].is<uint32_t>()) {
    partial.block_height = doc["blockheight"].as<uint32_t>();
  }
  // Fee-stream gating: only honour the topic we actually subscribed
  // to for the current `blockFeeDec` setting. This is defensive — a
  // stale relay-side subscription, an in-flight reconnect, or a
  // SetBlockFeeDec mid-stream can each leave the *other* topic
  // arriving for a tick or two; dispatching it would overwrite the
  // snapshot fee field with the wrong precision.
  if (!block_fee_dec_ && doc["blockfee"].is<int32_t>()) {
    partial.block_fee = doc["blockfee"].as<int32_t>();
  }
  if (block_fee_dec_ && doc["blockfee2"].is<double>()) {
    partial.block_fee_precise = doc["blockfee2"].as<double>();
  }

  // Price frames: {"price": {"USD": "64211.53", …}}. The server may send
  // the value as a string (preferred, preserves precision) or a number;
  // we accept both and normalise to a string for the snapshot.
  JsonVariant price = doc["price"];
  if (price.is<JsonObject>()) {
    for (JsonPair kv : price.as<JsonObject>()) {
      std::string val;
      if (kv.value().is<const char*>()) {
        val = kv.value().as<const char*>();
      } else if (kv.value().is<double>()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", kv.value().as<double>());
        val = buf;
      } else if (kv.value().is<int>() || kv.value().is<long long>()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(kv.value().as<long long>()));
        val = buf;
      }
      if (!val.empty()) {
        partial.prices[kv.key().c_str()] = std::move(val);
      }
    }
  }

  if (hub_) hub_->Report(partial);
}

}  // namespace btclock
