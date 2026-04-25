#include "sources/mempool_kraken_source.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "cJSON.h"
#include "data_core/hub.hpp"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "mempool+kraken";
constexpr const char* kMempoolUri = "wss://mempool.space/api/v1/ws";
constexpr const char* kKrakenUri = "wss://ws.kraken.com/v2";

// Format a double as a fixed-point string with the given decimals,
// matching the v2 source's homogeneous string-price contract. We
// renderer-side parse back to double, but keeping the snapshot stringy
// preserves Kraken's reported precision.
std::string FormatPrice(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return std::string(buf);
}

// Pull a number out of a cJSON node regardless of whether the upstream
// encoded it as a JSON number or a JSON string. Kraken sends most
// numerics as numbers; mempool.space is uniformly numbers; but the
// Kraken docs warn fields can shift representation across releases.
bool AsDouble(const cJSON* n, double* out) {
  if (n == nullptr) return false;
  if (cJSON_IsNumber(n)) {
    *out = n->valuedouble;
    return true;
  }
  if (cJSON_IsString(n) && n->valuestring != nullptr) {
    char* end = nullptr;
    const double v = std::strtod(n->valuestring, &end);
    if (end != n->valuestring) {
      *out = v;
      return true;
    }
  }
  return false;
}

}  // namespace

MempoolKrakenSource::MempoolKrakenSource(std::vector<std::string> currencies,
                                         bool block_fee_dec)
    : currencies_(std::move(currencies)), block_fee_dec_(block_fee_dec) {}

MempoolKrakenSource::~MempoolKrakenSource() { Stop(); }

esp_err_t MempoolKrakenSource::Start(DataHub& hub) {
  hub_ = &hub;

  // --- mempool.space client ---------------------------------------------
  {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = kMempoolUri;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 30;
    cfg.pingpong_timeout_sec = 20;
    // Mempool's initial `mempool-blocks` + `blocks` snapshot lands as a
    // single ~10–14 KB frame; oversize the rx buffer so the frame fits
    // in one event (also matches the bumped TLS IN_CONTENT_LEN so a
    // single TLS record carries the whole WS frame). Same root issue
    // the v3 firmware patched ArduinoWebsockets for.
    cfg.buffer_size = 16384;
    cfg.task_stack = 6144;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    mempool_client_ = esp_websocket_client_init(&cfg);
    if (mempool_client_ == nullptr) {
      ESP_LOGE(kTag, "mempool client init failed");
      return ESP_FAIL;
    }
    if (esp_websocket_register_events(mempool_client_, WEBSOCKET_EVENT_ANY,
                                      &MempoolEventTrampoline,
                                      this) != ESP_OK) {
      ESP_LOGE(kTag, "mempool register_events failed");
    }
    ESP_LOGI(kTag, "mempool connecting: %s", kMempoolUri);
    const esp_err_t rc = esp_websocket_client_start(mempool_client_);
    if (rc != ESP_OK) {
      ESP_LOGW(kTag, "mempool start: %s", esp_err_to_name(rc));
      // Don't bail — kraken might still come up; the built-in retry
      // loop will reconnect when the network recovers.
    }
  }

  // --- kraken v2 client -------------------------------------------------
  {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = kKrakenUri;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 30;
    cfg.pingpong_timeout_sec = 20;
    // Kraken ticker frames are smaller than mempool's, but the V2 docs
    // warn snapshot frames on first subscribe can carry per-currency
    // depth — 4 KB is comfortable.
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    kraken_client_ = esp_websocket_client_init(&cfg);
    if (kraken_client_ == nullptr) {
      ESP_LOGE(kTag, "kraken client init failed");
      // Mempool keeps running even if kraken init fails.
    } else {
      if (esp_websocket_register_events(kraken_client_, WEBSOCKET_EVENT_ANY,
                                        &KrakenEventTrampoline,
                                        this) != ESP_OK) {
        ESP_LOGE(kTag, "kraken register_events failed");
      }
      ESP_LOGI(kTag, "kraken connecting: %s", kKrakenUri);
      const esp_err_t rc = esp_websocket_client_start(kraken_client_);
      if (rc != ESP_OK) {
        ESP_LOGW(kTag, "kraken start: %s", esp_err_to_name(rc));
      }
    }
  }

  return ESP_OK;
}

esp_err_t MempoolKrakenSource::Stop() {
  if (mempool_client_ != nullptr) {
    esp_websocket_client_stop(mempool_client_);
    esp_websocket_client_destroy(mempool_client_);
    mempool_client_ = nullptr;
  }
  if (kraken_client_ != nullptr) {
    esp_websocket_client_stop(kraken_client_);
    esp_websocket_client_destroy(kraken_client_);
    kraken_client_ = nullptr;
  }
  mempool_connected_.store(false);
  kraken_connected_.store(false);
  hub_ = nullptr;
  return ESP_OK;
}

// ---- mempool event handlers -------------------------------------------

void MempoolKrakenSource::MempoolEventTrampoline(void* arg, esp_event_base_t,
                                                 int32_t event_id,
                                                 void* event_data) {
  static_cast<MempoolKrakenSource*>(arg)->HandleMempoolEvent(event_id,
                                                             event_data);
}

void MempoolKrakenSource::HandleMempoolEvent(int32_t event_id,
                                             void* event_data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      mempool_connected_.store(true);
      ESP_LOGI(kTag, "mempool ws connected");
      mempool_frame_.clear();
      SendMempoolSubscriptions();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      mempool_connected_.store(false);
      ESP_LOGW(kTag, "mempool ws disconnected");
      mempool_frame_.clear();
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 0x1 = text; 0x0 (continuation) for fragmented payloads.
      // Mempool sends only text frames, but the library can split a
      // single logical frame into multiple WEBSOCKET_EVENT_DATA events
      // when payload_len > buffer_size; reassemble using payload_offset
      // and payload_len.
      if (ev && (ev->op_code == 0x1 || ev->op_code == 0x0) &&
          ev->data_len > 0) {
        if (ev->payload_offset == 0) mempool_frame_.clear();
        mempool_frame_.append(ev->data_ptr, ev->data_len);
        const int total = ev->payload_offset + ev->data_len;
        if (total >= ev->payload_len) {
          HandleMempoolFrame(mempool_frame_.data(), mempool_frame_.size());
          mempool_frame_.clear();
        }
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      mempool_connected_.store(false);
      ESP_LOGW(kTag, "mempool ws error");
      break;
    default:
      break;
  }
}

void MempoolKrakenSource::SendMempoolSubscriptions() {
  // Mempool subscribe is a single `want` frame listing the topics. The
  // server responds with the current snapshot for each topic and then
  // streams updates. `mempool-blocks` carries the next-block fee
  // estimator (the legacy `blockfee2` parity field).
  static constexpr const char* kSub =
      "{\"action\":\"want\",\"data\":[\"blocks\",\"mempool-blocks\","
      "\"fees\"]}";
  const int n = esp_websocket_client_send_text(
      mempool_client_, kSub, std::strlen(kSub), pdMS_TO_TICKS(2000));
  if (n < 0) {
    ESP_LOGW(kTag, "mempool subscribe send failed");
  } else {
    ESP_LOGI(kTag, "mempool subscribed (blocks+mempool-blocks+fees)");
  }
}

void MempoolKrakenSource::HandleMempoolFrame(const char* data, size_t len) {
  // cJSON_ParseWithLength tolerates non-NUL-terminated buffers — perfect
  // for the WS frame buffer.
  cJSON* root = cJSON_ParseWithLength(data, len);
  if (root == nullptr) {
    ESP_LOGW(kTag, "mempool: json parse failed (%u bytes)",
             static_cast<unsigned>(len));
    return;
  }

  DataSnapshot partial;
  bool reported = false;

  // `block` (singular) — single new confirmed block. `blocks` (plural)
  // arrives in the initial snapshot as an array of recent blocks; we
  // take the first/newest as the current tip.
  cJSON* block = cJSON_GetObjectItemCaseSensitive(root, "block");
  if (cJSON_IsObject(block)) {
    cJSON* h = cJSON_GetObjectItemCaseSensitive(block, "height");
    if (cJSON_IsNumber(h)) {
      partial.block_height =
          static_cast<uint32_t>(h->valuedouble);
    }
  }
  cJSON* blocks = cJSON_GetObjectItemCaseSensitive(root, "blocks");
  if (cJSON_IsArray(blocks)) {
    cJSON* first = cJSON_GetArrayItem(blocks, 0);
    if (cJSON_IsObject(first)) {
      cJSON* h = cJSON_GetObjectItemCaseSensitive(first, "height");
      if (cJSON_IsNumber(h) && !partial.block_height) {
        partial.block_height =
            static_cast<uint32_t>(h->valuedouble);
      }
    }
  }

  // `fees` — recommended fee estimator. Map to block_fee/_precise so
  // existing renderers see the same wire-shape as the v2 path. We use
  // `halfHourFee` (medium priority) as the "current rate" — same choice
  // the v3 firmware made for parity.
  cJSON* fees = cJSON_GetObjectItemCaseSensitive(root, "fees");
  if (cJSON_IsObject(fees)) {
    cJSON* mid = cJSON_GetObjectItemCaseSensitive(fees, "halfHourFee");
    double f = 0.0;
    if (AsDouble(mid, &f)) {
      partial.block_fee = static_cast<int32_t>(std::lround(f));
      if (block_fee_dec_) partial.block_fee_precise = f;
    }
  }

  // `mempool-blocks` — projected blocks from the current mempool. Entry
  // [0] is the next block; its `medianFee` is the canonical "next-block
  // fee" reading. We prefer this over `fees.halfHourFee` for the
  // precise-decimal path because medianFee carries float precision.
  cJSON* mb = cJSON_GetObjectItemCaseSensitive(root, "mempool-blocks");
  if (cJSON_IsArray(mb)) {
    cJSON* next = cJSON_GetArrayItem(mb, 0);
    if (cJSON_IsObject(next)) {
      cJSON* mf = cJSON_GetObjectItemCaseSensitive(next, "medianFee");
      double f = 0.0;
      if (AsDouble(mf, &f)) {
        if (block_fee_dec_) {
          partial.block_fee_precise = f;
        } else {
          partial.block_fee = static_cast<int32_t>(std::lround(f));
        }
      }
    }
  }

  if (hub_ != nullptr &&
      (partial.block_height || partial.block_fee ||
       partial.block_fee_precise)) {
    hub_->Report(partial);
    reported = true;
  }
  (void)reported;

  cJSON_Delete(root);
}

// ---- kraken event handlers --------------------------------------------

void MempoolKrakenSource::KrakenEventTrampoline(void* arg, esp_event_base_t,
                                                int32_t event_id,
                                                void* event_data) {
  static_cast<MempoolKrakenSource*>(arg)->HandleKrakenEvent(event_id,
                                                            event_data);
}

void MempoolKrakenSource::HandleKrakenEvent(int32_t event_id,
                                            void* event_data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      kraken_connected_.store(true);
      ESP_LOGI(kTag, "kraken ws connected");
      kraken_frame_.clear();
      SendKrakenSubscriptions();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      kraken_connected_.store(false);
      ESP_LOGW(kTag, "kraken ws disconnected");
      kraken_frame_.clear();
      break;
    case WEBSOCKET_EVENT_DATA:
      if (ev && (ev->op_code == 0x1 || ev->op_code == 0x0) &&
          ev->data_len > 0) {
        if (ev->payload_offset == 0) kraken_frame_.clear();
        kraken_frame_.append(ev->data_ptr, ev->data_len);
        const int total = ev->payload_offset + ev->data_len;
        if (total >= ev->payload_len) {
          HandleKrakenFrame(kraken_frame_.data(), kraken_frame_.size());
          kraken_frame_.clear();
        }
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      kraken_connected_.store(false);
      ESP_LOGW(kTag, "kraken ws error");
      break;
    default:
      break;
  }
}

void MempoolKrakenSource::SendKrakenSubscriptions() {
  // Kraken V2 takes one subscribe with a `symbol` array. Build
  // ["BTC/USD","BTC/EUR",…] from `currencies_`. V2 uses BTC/* (V1 used
  // XBT/*) — see https://docs.kraken.com/api/docs/websocket-v2/ticker.
  // Skip empty currency lists — sending an empty array yields a server
  // error and the loop attempts to send it on every reconnect.
  if (currencies_.empty()) {
    ESP_LOGW(kTag, "kraken: no currencies; skipping subscribe");
    return;
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "method", "subscribe");
  cJSON* params = cJSON_AddObjectToObject(root, "params");
  cJSON_AddStringToObject(params, "channel", "ticker");
  cJSON* syms = cJSON_AddArrayToObject(params, "symbol");
  for (const auto& ccy : currencies_) {
    std::string sym = "BTC/";
    sym.append(ccy);
    cJSON_AddItemToArray(syms, cJSON_CreateString(sym.c_str()));
  }
  char* msg = cJSON_PrintUnformatted(root);
  if (msg != nullptr) {
    const int n = esp_websocket_client_send_text(
        kraken_client_, msg, std::strlen(msg), pdMS_TO_TICKS(2000));
    if (n < 0) {
      ESP_LOGW(kTag, "kraken subscribe send failed");
    } else {
      ESP_LOGI(kTag, "kraken subscribed: %s", msg);
    }
    cJSON_free(msg);
  }
  cJSON_Delete(root);
}

void MempoolKrakenSource::HandleKrakenFrame(const char* data, size_t len) {
  cJSON* root = cJSON_ParseWithLength(data, len);
  if (root == nullptr) {
    ESP_LOGW(kTag, "kraken: json parse failed (%u bytes)",
             static_cast<unsigned>(len));
    return;
  }

  // Frame shapes:
  //   {"channel":"ticker","type":"snapshot|update",
  //    "data":[{"symbol":"BTC/USD","last":..., "bid":..., "ask":..., ...}]}
  //   {"channel":"heartbeat"}                (ignore)
  //   {"channel":"status",...}               (ignore)
  //   {"method":"subscribe","success":true,...}    (ignore)
  cJSON* channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
  if (!cJSON_IsString(channel) ||
      std::strcmp(channel->valuestring, "ticker") != 0) {
    cJSON_Delete(root);
    return;
  }

  cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (!cJSON_IsArray(arr)) {
    cJSON_Delete(root);
    return;
  }

  DataSnapshot partial;
  cJSON* item = nullptr;
  cJSON_ArrayForEach(item, arr) {
    if (!cJSON_IsObject(item)) continue;
    cJSON* sym = cJSON_GetObjectItemCaseSensitive(item, "symbol");
    if (!cJSON_IsString(sym) || sym->valuestring == nullptr) continue;
    // "BTC/USD" -> "USD". Skip if the prefix isn't BTC/ — V2 also lets
    // clients subscribe to XBT/* aliases for back-compat, treat both.
    const char* slash = std::strchr(sym->valuestring, '/');
    if (slash == nullptr) continue;
    const std::string ccy(slash + 1);
    if (ccy.empty()) continue;

    // Prefer `last` (last-trade price); fall back to ticker midpoint
    // (bid+ask)/2 if `last` is missing — Kraken's snapshot can omit
    // `last` for a freshly-listed pair.
    cJSON* last = cJSON_GetObjectItemCaseSensitive(item, "last");
    double price = 0.0;
    if (!AsDouble(last, &price)) {
      cJSON* bid = cJSON_GetObjectItemCaseSensitive(item, "bid");
      cJSON* ask = cJSON_GetObjectItemCaseSensitive(item, "ask");
      double b = 0.0, a = 0.0;
      if (AsDouble(bid, &b) && AsDouble(ask, &a) && b > 0 && a > 0) {
        price = (b + a) * 0.5;
      } else {
        continue;
      }
    }
    if (price <= 0.0) continue;
    partial.prices[ccy] = FormatPrice(price);
  }

  if (hub_ != nullptr && !partial.prices.empty()) {
    hub_->Report(partial);
  }

  cJSON_Delete(root);
}

}  // namespace btclock
