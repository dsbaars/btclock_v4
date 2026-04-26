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

#include <functional>
#include <string>

#include "esp_err.h"
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

 private:
  static void EventTrampoline(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);

  std::string url_;
  esp_websocket_client_handle_t client_ = nullptr;
  TextCallback on_frame_;
  ConnectCallback on_connect_;
  volatile bool connected_ = false;
};

}  // namespace nostr
}  // namespace btclock
