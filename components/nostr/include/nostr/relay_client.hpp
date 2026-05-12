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

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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
  // Per-event metadata snapshot — useful when frames_chunk and
  // frames_complete drift apart without us knowing why. Read by the
  // /api/nwc/debug builder; written from the WS task on every
  // WEBSOCKET_EVENT_DATA event. Records the LAST event seen (whether
  // it ended a logical frame or not), so a comparison against
  // frames_chunk tells you "is the last event still mid-frame".
  uint8_t last_evt_op_code() const { return last_evt_op_code_.load(); }
  uint8_t last_evt_fin() const { return last_evt_fin_.load(); }
  int32_t last_evt_payload_offset() const {
    return last_evt_payload_offset_.load();
  }
  int32_t last_evt_payload_len() const { return last_evt_payload_len_.load(); }
  int32_t last_evt_data_len() const { return last_evt_data_len_.load(); }
  // First 96 bytes of the LAST emitted (post-reassembly) frame. Mostly
  // useful to confirm "did the reassembled buffer start with '['" —
  // a tail-only buffer is the smoking gun for fragment-reassembly
  // bugs. Caller copies under mu_ since it's a std::string.
  std::string last_emitted_head() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_emitted_head_;
  }

  // Ring buffer of the most recent N raw WS event metadata records.
  // Per record: op_code, fin, payload_offset, payload_len, data_len,
  // a frame-sequence id (monotonic per-event counter), and a
  // 1-bit flag `emit` indicating whether this event completed a
  // logical frame (i.e. on_frame_ fired). This is the diagnostic
  // surface for tracking the fragment-reassembly bug — by comparing
  // payload_offset/payload_len/data_len across N consecutive events
  // you can tell whether esp_websocket_client is splitting a single
  // logical WS frame across calls, and where our accumulator drops
  // the head.
  struct EvtRecord {
    uint32_t seq = 0;
    uint8_t op_code = 0;
    uint8_t fin = 0;
    uint8_t emit = 0;
    int32_t payload_offset = 0;
    int32_t payload_len = 0;
    int32_t data_len = 0;
  };
  static constexpr size_t kEvtHistory = 16;
  // Returns the most recent up-to-kEvtHistory records, oldest first.
  std::vector<EvtRecord> evt_history() const;

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
  std::atomic<uint8_t> last_evt_op_code_{0};
  std::atomic<uint8_t> last_evt_fin_{0};
  std::atomic<int32_t> last_evt_payload_offset_{0};
  std::atomic<int32_t> last_evt_payload_len_{0};
  std::atomic<int32_t> last_evt_data_len_{0};
  mutable std::mutex mu_;          // guards last_emitted_head_ + evt_history_
  std::string last_emitted_head_;  // first ≤ 96 bytes after reassembly
  std::array<EvtRecord, kEvtHistory> evt_history_{};
  size_t evt_history_pos_ = 0;
  size_t evt_history_filled_ = 0;
  uint32_t evt_seq_ = 0;
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
