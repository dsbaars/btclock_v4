// Server-Sent Events (`/events`) for the control API.
//
// The WebUI relies on a long-lived `GET /events` stream to refresh
// the status panel without polling: the Arduino firmware does this via
// ESPAsyncWebServer's `AsyncEventSource`. There is no equivalent object
// on `esp_http_server`, so we roll our own using the primitives the IDF
// does expose:
//
//   * `httpd_req_async_handler_begin` to detach a GET handler and keep
//     the underlying socket open past the return of the URI handler
//     callback.
//   * `httpd_resp_send_chunk` to write each `event: …\ndata: …\n\n`
//     frame on the stream.
//   * A background task owned by this component to broadcast and
//     heartbeat: per-client writes serialise through a mutex so a
//     /api call from the REST worker thread can safely ask us to push.
//
// Thread model:
//   * `Broadcast*` methods are safe to call from any FreeRTOS task —
//     they grab the client-list mutex and fan out to every open stream
//     inline. No `httpd_queue_work` indirection: we're already on a
//     worker task (or the main task) and doing the send from that task
//     keeps latency low without the extra hop.
//   * The detached async `req*` per client is only written on the task
//     that holds the mutex, so concurrent writes can't interleave.
//
// What's intentionally NOT in this component:
//   * The status JSON itself. That builder lives next to the other
//     /api/status code in control_server.cpp so there's one source of
//     truth; we just take a pre-serialised string.
//   * Auth policy decisions. `HandleEvents` calls `RequireHttpAuth`
//     before the async detach so SSE is gated identically to the rest
//     of /api/*. The helper itself lives next to the REST-side gate
//     in `auth_gate.hpp`.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sse_frame.hpp"

namespace btclock {

class SseServer {
 public:
  struct Config {
    // How many concurrent clients to accept. Each SSE client holds a
    // TCP socket for its entire lifetime, so this can't exceed the
    // httpd's max_open_sockets minus the reserved headroom (3 per
    // IDF's config comment). 4 is enough for a few WebUI tabs plus
    // one CLI consumer without starving the REST surface.
    size_t max_clients = 4;
    // Keep-alive cadence. Browsers and corporate proxies often kill
    // silent TCP connections after 30-60 s; 5 s comfortably beats that
    // and is cheap (tiny comment frame).
    uint32_t heartbeat_ms = 5000;
  };

  explicit SseServer(Config cfg);
  ~SseServer();

  SseServer(const SseServer&) = delete;
  SseServer& operator=(const SseServer&) = delete;

  // Register `GET /events` on an already-started httpd. Caller retains
  // ownership of the httpd handle.
  esp_err_t RegisterRoute(httpd_handle_t server);

  // Broadcast a frame to every connected client. Thread-safe. Returns
  // the number of clients the frame was successfully sent to; silently
  // prunes any client whose send errored (socket closed mid-stream).
  size_t Broadcast(std::string_view event_name, std::string_view data);

  // Current connected-client count. Cheap; for diagnostics and tests.
  size_t ClientCount() const;

 private:
  struct Client {
    httpd_req_t* req;       // detached async request
    int64_t last_heartbeat_us;
  };

  static esp_err_t TrampolineEvents(httpd_req_t* req);
  esp_err_t HandleEvents(httpd_req_t* req);

  // Send a frame to one client. Must be called with `mu_` held.
  // Returns false if the socket write failed — caller should evict.
  bool SendFrameLocked(Client& c, std::string_view frame);

  // Evict + free an entry by index. Must be called with `mu_` held.
  void EvictLocked(size_t idx);

  // Heartbeat loop. Started on first client connect; stops when the
  // last client disconnects.
  static void HeartbeatTaskTrampoline(void* arg);
  void HeartbeatLoop();

  Config cfg_;
  httpd_handle_t server_ = nullptr;

  mutable std::mutex mu_;
  std::vector<Client> clients_;

  // The heartbeat task exists for the full SseServer lifetime once
  // started (cheap — sleeps most of the time). Self-terminates via
  // `shutdown_` in the destructor.
  TaskHandle_t heartbeat_task_ = nullptr;
  volatile bool shutdown_ = false;
};

}  // namespace btclock
