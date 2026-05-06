#include "btclock_data.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ArduinoJson.h"
#include "btclock_lastblock_uri.hpp"
#include "btclock_subscribe.hpp"
#include "data_core/hub.hpp"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_transport_ws.h"
#include "freertos/task.h"
#include "proxy_transport/proxy_prefs.hpp"
#include "proxy_transport/proxy_transport.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock-data";

// Watchdog cadence and threshold.
//
// Tick every 5 min so a recovery from a relay outage typically lands
// inside one block-interval window even when the staleness threshold
// itself is long. The threshold is 60 min — block intervals are
// exponentially distributed so ~3 % of natural intervals exceed 60 min
// even on a perfectly healthy connection. That's fine: when we fire on
// a healthy gap, the HTTP probe sees the same height as the snapshot
// and we don't reconnect.
constexpr uint32_t kWatchdogTickMs = 5 * 60 * 1000;
constexpr uint32_t kStaleThresholdMs = 60 * 60 * 1000;

// /api/lastblock returns the integer height as plain text (e.g.
// "947195\n"). 32 bytes is generous headroom; if the body grows past
// that the server changed shape and we should refuse to parse rather
// than reconnect on garbage.
constexpr std::size_t kLastblockMaxBytes = 32;

struct LastblockFetchCtx {
  char buf[kLastblockMaxBytes + 1] = {};
  std::size_t size = 0;
  bool truncated = false;
};

esp_err_t LastblockHttpEvent(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<LastblockFetchCtx*>(evt->user_data);
  if (ctx == nullptr || evt->data == nullptr || evt->data_len <= 0) {
    return ESP_OK;
  }
  const std::size_t incoming = static_cast<std::size_t>(evt->data_len);
  if (ctx->size + incoming > kLastblockMaxBytes) {
    ctx->truncated = true;
    return ESP_OK;
  }
  std::memcpy(ctx->buf + ctx->size, evt->data, incoming);
  ctx->size += incoming;
  return ESP_OK;
}
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

  // Build the proxy-aware ext_transport chain. With proxyEnabled=false
  // the inner transport bypasses straight to a direct TCP+TLS connect
  // to the destination, so behaviour matches the pre-migration path
  // byte-for-byte. When proxyEnabled=true the inner transport runs the
  // SOCKS/HTTP CONNECT handshake first, then esp_tls handshakes against
  // the *real* destination's cert (SNI = uri_).
  {
    btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
    const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
    proxy_inner_ = btclock::proxy::MakeProxyTransport(
        proxy_cfg, btclock::proxy::ParamsForUrl(uri_, esp_crt_bundle_attach));
    proxy_ws_ = esp_transport_ws_init(proxy_inner_);
    if (proxy_ws_) {
      // Mirrors esp_websocket_client_create_transport's default-port
      // call on the auto-built path; required because ext_transport
      // skips that wiring and the reconnect logic uses default_port to
      // fill in port=0 for URIs without an explicit port.
      esp_transport_set_default_port(
          proxy_ws_, btclock::proxy::UrlImpliesTls(uri_) ? 443 : 80);
    }
  }
  if (!proxy_ws_) {
    if (proxy_inner_) {
      esp_transport_destroy(proxy_inner_);
      proxy_inner_ = nullptr;
    }
    return ESP_FAIL;
  }
  // The WS client skips its internal optional-settings apply when
  // ext_transport is non-null (esp_websocket_client.c:582,647), so push
  // the path explicitly. propagate_control_frames mirrors the value the
  // client would have set on the auto-built path.
  const std::string ws_path = btclock::proxy::PathFromUri(uri_);
  esp_transport_ws_config_t ws_cfg = {};
  ws_cfg.ws_path = ws_path.c_str();
  ws_cfg.propagate_control_frames = true;
  esp_transport_ws_set_config(proxy_ws_, &ws_cfg);

  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri_.c_str();
  cfg.reconnect_timeout_ms = 5000;  // linear backoff, 5 s between retries
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 20;
  cfg.pingpong_timeout_sec = 15;
  cfg.buffer_size = 4096;  // headroom for price map frames
  cfg.task_stack = 6144;
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
                                    &EventHandlerTrampoline, this) == ESP_OK,
      ESP_FAIL, kTag, "register_events");

  // Mirror the nostr-relay log shape ("connecting: %s") so a user
  // grepping the serial console can spot both ws clients with the same
  // pattern.
  ESP_LOGI(kTag, "connecting: %s", uri_.c_str());
  const esp_err_t start_err = esp_websocket_client_start(client_);
  if (start_err != ESP_OK) return start_err;

  // Reset watchdog state on (re)start so we don't fire immediately on
  // a fresh connect — give the WS a fair window to deliver a tip
  // before the timer counts that delivery as "stale". last_height_ is
  // intentionally NOT cleared: a Stop()+Start() bounce (currency or
  // fee change) shouldn't lose the cached height the renderers are
  // painting from.
  last_change_tick_.store(xTaskGetTickCount(), std::memory_order_release);
  if (watchdog_timer_ == nullptr) {
    watchdog_timer_ = xTimerCreate("btclock-wd", pdMS_TO_TICKS(kWatchdogTickMs),
                                   pdTRUE /* auto-reload */, this,
                                   &BtclockDataSource::WatchdogTrampoline);
    if (watchdog_timer_ == nullptr) {
      ESP_LOGW(kTag, "watchdog timer create failed; staleness recovery off");
    }
  }
  if (watchdog_timer_ != nullptr) {
    xTimerStart(watchdog_timer_, 0);
  }
  return ESP_OK;
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
  // Tear the watchdog down before the WS so a late-firing tick can't
  // race a half-destroyed client handle. Stop() is idempotent — a
  // SetCurrencies()/SetBlockFeeDec() bounce reaches Stop() then Start()
  // back-to-back and recreates the timer fresh.
  if (watchdog_timer_ != nullptr) {
    xTimerStop(watchdog_timer_, 0);
    xTimerDelete(watchdog_timer_, 0);
    watchdog_timer_ = nullptr;
  }
  // A probe worker may be mid-fetch on its own task — wait for it to
  // finish before destroying the WS client, otherwise its
  // ForceReconnect() / RunStalenessProbe() would touch a freed handle.
  // Bounded by FetchUpstreamHeight's HTTP timeout (8 s) plus margin.
  for (int i = 0; i < 100 && probe_in_flight_.load(std::memory_order_acquire);
       ++i) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (client_ == nullptr) {
    hub_ = nullptr;
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
    const uint32_t h = doc["blockheight"].as<uint32_t>();
    partial.block_height = h;
    // Watchdog: only reset the staleness clock when the height
    // *changes*. Relays do re-broadcast the current tip on subscribe
    // and occasionally as a keepalive, and we don't want those to mask
    // a stalled subscription stream.
    const uint32_t prev = last_height_.exchange(h, std::memory_order_acq_rel);
    if (prev != h) {
      last_change_tick_.store(xTaskGetTickCount(), std::memory_order_release);
    }
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

void BtclockDataSource::WatchdogTrampoline(TimerHandle_t timer) {
  auto* self = static_cast<BtclockDataSource*>(pvTimerGetTimerID(timer));
  if (self != nullptr) self->OnWatchdogTick();
}

void BtclockDataSource::OnWatchdogTick() {
  // Runs on FreeRTOS Tmr Svc — 2048-word stack. Anything HTTPS-shaped
  // (esp_http_client + TLS handshake + cert bundle) blows that stack
  // and panics with "stack overflow in task Tmr Svc". So this callback
  // does only the cheap atomic staleness check and, on a hit, hands
  // off to a worker task with a real stack.
  if (client_ == nullptr) return;
  const TickType_t now = xTaskGetTickCount();
  const TickType_t since_tick =
      last_change_tick_.load(std::memory_order_acquire);
  if (since_tick == 0) return;  // no sample yet; nothing to compare
  const uint32_t since_ms = (now - since_tick) * portTICK_PERIOD_MS;
  if (since_ms < kStaleThresholdMs) return;

  // Skip if a probe is already running so a backed-up timer queue can't
  // pile up workers while one is mid-handshake.
  bool expected = false;
  if (!probe_in_flight_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
    return;
  }
  // 6 KiB stack mirrors the WS client's task_stack — same TLS path.
  if (xTaskCreate(&BtclockDataSource::ProbeTaskTrampoline, "btclock-wd-probe",
                  6144, this, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    probe_in_flight_.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "watchdog: probe task spawn failed");
  }
}

void BtclockDataSource::ProbeTaskTrampoline(void* arg) {
  auto* self = static_cast<BtclockDataSource*>(arg);
  self->RunStalenessProbe();
  self->probe_in_flight_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void BtclockDataSource::RunStalenessProbe() {
  // Re-check client presence — Stop() may have nulled it between Tmr
  // Svc spawning us and our first instruction here.
  if (client_ == nullptr) return;

  uint32_t upstream = 0;
  if (!FetchUpstreamHeight(upstream)) {
    // Graceful: HTTP failure is not a reconnect signal. Could be DNS,
    // a captive portal, transient TLS failure, or the custom endpoint
    // not exposing /api/lastblock. Try again on the next tick.
    ESP_LOGW(kTag, "watchdog: lastblock probe failed; deferring reconnect");
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  const TickType_t since_tick =
      last_change_tick_.load(std::memory_order_acquire);
  const uint32_t since_ms = (now - since_tick) * portTICK_PERIOD_MS;

  const uint32_t cached = last_height_.load(std::memory_order_acquire);
  if (cached == upstream) {
    // Healthy gap: the network really hadn't produced a block in 60+
    // min. Reset the clock so we don't re-probe every tick until the
    // next block lands.
    ESP_LOGI(kTag, "watchdog: %u min idle but tip matches upstream (%u); ok",
             static_cast<unsigned>(since_ms / 60000),
             static_cast<unsigned>(upstream));
    last_change_tick_.store(now, std::memory_order_release);
    return;
  }

  ESP_LOGW(kTag,
           "watchdog: stale (%u min idle, cached=%u upstream=%u); reconnecting",
           static_cast<unsigned>(since_ms / 60000),
           static_cast<unsigned>(cached), static_cast<unsigned>(upstream));
  ForceReconnect();
  // Reset the clock around the reconnect so the next tick gives the
  // fresh session a full window before re-evaluating.
  last_change_tick_.store(xTaskGetTickCount(), std::memory_order_release);
}

bool BtclockDataSource::FetchUpstreamHeight(uint32_t& out) const {
  const std::string url = BuildLastblockUri(uri_);
  if (url.empty()) return false;

  LastblockFetchCtx ctx;
  btclock::settings::NvsPrefs proxy_prefs(btclock::prefs::kSettingsNs);
  const auto proxy_cfg = btclock::proxy::LoadConfigFromPrefs(proxy_prefs);
  btclock::proxy::OwnedTransport proxy_t(btclock::proxy::MakeProxyTransport(
      proxy_cfg, btclock::proxy::ParamsForUrl(url, esp_crt_bundle_attach)));

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &LastblockHttpEvent;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport = proxy_t.get();

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;

  bool ok = false;
  do {
    if (esp_http_client_perform(client) != ESP_OK) break;
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) break;
    if (ctx.truncated || ctx.size == 0) break;
    ctx.buf[ctx.size] = '\0';
    char* end = nullptr;
    const long parsed = std::strtol(ctx.buf, &end, 10);
    if (end == ctx.buf || parsed <= 0) break;
    out = static_cast<uint32_t>(parsed);
    ok = true;
  } while (false);

  esp_http_client_cleanup(client);
  return ok;
}

void BtclockDataSource::ForceReconnect() {
  if (client_ == nullptr) return;
  // close() is the lighter sibling of stop()/destroy(): it tears down
  // the TCP/TLS layer but keeps the client object and event handlers
  // intact, so the built-in linear backoff fires CONNECTED again ~5 s
  // later and SendSubscriptions() replays automatically.
  esp_websocket_client_close(client_, pdMS_TO_TICKS(2000));
}

}  // namespace btclock
