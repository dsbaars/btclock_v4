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
  // Per-WS-event counter increments. frames_complete_ counts logical
  // frames forwarded to on_frame_; frames_chunk_ counts every raw
  // WEBSOCKET_EVENT_DATA we saw. A frames_chunk_ that climbs without
  // a matching frames_complete_ implicates the fragmentation
  // accumulator. last_frame_bytes_ records the size of the last
  // complete frame delivered upstream — useful for spotting
  // "single-chunk event landed but parser dropped it" cases.
  uint32_t frames_chunk() const { return frames_chunk_.load(); }
  uint32_t frames_complete() const { return frames_complete_.load(); }
  uint32_t last_frame_bytes() const { return last_frame_bytes_.load(); }

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
  std::atomic<uint32_t> frames_chunk_{0};
  std::atomic<uint32_t> frames_complete_{0};
  std::atomic<uint32_t> last_frame_bytes_{0};
  // Accumulator for fragmented text frames. esp_websocket_client
  // splits a single logical WS frame into multiple
  // WEBSOCKET_EVENT_DATA events when the payload exceeds the
  // internal TCP segment buffer — small frames (kind 13194 INFO,
  // kind 23195 get_balance responses) fit in one chunk, but large
  // ones (kind 23197 / 23196 payment notifications, large zap
  // receipts) fragment. Reassembled here and forwarded to on_frame_
  // only when complete. Only touched on the WS task, no mutex.
  std::string fragment_buf_;
};

}  // namespace nostr
}  // namespace btclock
