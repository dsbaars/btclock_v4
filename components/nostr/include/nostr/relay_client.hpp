// Nostr relay WebSocket client.
//
// One instance per relay URL. Wraps `esp_websocket_client` — reconnect
// and linear backoff come from the underlying client (same pattern used
// in components/btclock_data/). The caller installs a text-frame
// callback; we forward every server-pushed text frame to it on the
// websocket task. Sends are queued through the underlying client's
// internal send buffer and time out after 2 s per frame.
//
// Threading: callbacks run on the esp_websocket task, same as the
// btclock WS v2 source. Keep them fast; stash into a queue / task
// notification for any heavy work.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"
#include "esp_transport.h"
#include "esp_websocket_client.h"

namespace btclock {
namespace nostr {

class RelayClient {
 public:
  using TextCallback = std::function<void(const char* data, size_t len)>;
  using ConnectCallback = std::function<void(bool connected)>;

  explicit RelayClient(std::string url);
  ~RelayClient();

  RelayClient(const RelayClient&) = delete;
  RelayClient& operator=(const RelayClient&) = delete;

  // Set before Start(). Callbacks fire on the websocket task.
  void SetOnFrame(TextCallback cb) { on_frame_ = std::move(cb); }
  void SetOnConnect(ConnectCallback cb) { on_connect_ = std::move(cb); }

  esp_err_t Start();
  esp_err_t Stop();

  // Send a UTF-8 text frame. Thread-safe.
  bool SendText(const char* data, size_t len);
  bool SendText(const std::string& s) { return SendText(s.data(), s.size()); }

  bool connected() const { return connected_; }
  const std::string& url() const { return url_; }

  // Debug counters — fed by /api/nwc/debug (NwcClient surface) to
  // attribute "events never reach the device" to either WSS churn or
  // higher-layer drops. All atomics so the HTTP task can read without
  // racing the websocket task. Timestamps are monotonic ms since boot
  // (esp_timer_get_time()/1000); 0 means "never observed".
  uint32_t reconnect_count() const { return reconnect_count_.load(); }
  int64_t last_connect_ms() const { return last_connect_ms_.load(); }
  int64_t last_disconnect_ms() const { return last_disconnect_ms_.load(); }

 private:
  static void EventTrampoline(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);

  std::string url_;
  esp_websocket_client_handle_t client_ = nullptr;
  // Proxy chain handed to client_ via cfg.ext_transport. The WS client
  // does not own ext_transport when it is set, so we destroy both
  // handles in Stop() in reverse-construction order.
  esp_transport_handle_t proxy_inner_ = nullptr;
  esp_transport_handle_t proxy_ws_ = nullptr;
  TextCallback on_frame_;
  ConnectCallback on_connect_;
  volatile bool connected_ = false;
  std::atomic<uint32_t> reconnect_count_{0};
  std::atomic<int64_t> last_connect_ms_{0};
  std::atomic<int64_t> last_disconnect_ms_{0};
};

}  // namespace nostr
}  // namespace btclock
