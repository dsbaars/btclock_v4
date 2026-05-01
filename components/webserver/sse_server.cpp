// SSE server — per-client async request lifecycle, broadcast fan-out,
// heartbeat keep-alive. See sse_server.hpp for the design rationale.

#include "sse_server.hpp"

#include <algorithm>

#include "auth_gate.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace btclock {
namespace {
constexpr const char* kTag = "sse";
constexpr const char* kWelcomeEvent = "welcome";
constexpr const char* kWelcomePayload = "{}";
}  // namespace

SseServer::SseServer(Config cfg) : cfg_(cfg) {
  clients_.reserve(cfg_.max_clients);
}

SseServer::~SseServer() {
  shutdown_ = true;
  // Wake the heartbeat task so it can observe `shutdown_` and exit
  // promptly rather than waiting for its next scheduled tick.
  if (heartbeat_task_) xTaskNotifyGive(heartbeat_task_);

  // Close and free every client. After the httpd_handle_t behind
  // `server_` is stopped (by the owner), the async req structures
  // still need `httpd_req_async_handler_complete` for lwip to
  // release the socket — the IDF docs explicitly warn that skipping
  // it exhausts the accept pool.
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& c : clients_) {
    if (c.req) httpd_req_async_handler_complete(c.req);
  }
  clients_.clear();
}

esp_err_t SseServer::RegisterRoute(httpd_handle_t server) {
  server_ = server;
  const httpd_uri_t entry = {.uri = "/events",
                             .method = HTTP_GET,
                             .handler = &SseServer::TrampolineEvents,
                             .user_ctx = this};
  return httpd_register_uri_handler(server, &entry);
}

size_t SseServer::ClientCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return clients_.size();
}

esp_err_t SseServer::TrampolineEvents(httpd_req_t* req) {
  return static_cast<SseServer*>(req->user_ctx)->HandleEvents(req);
}

esp_err_t SseServer::HandleEvents(httpd_req_t* req) {
  // Auth runs in the sync dispatch phase — before async detach — so
  // the 401 goes out on the regular (non-chunked) response path and
  // the browser's EventSource reconnect loop sees a clean status.
  // Mid-stream re-auth isn't a concept for SSE: once connected the
  // socket stays open for the session's lifetime.
  if (!RequireHttpAuth(req)) return ESP_OK;

  {
    std::lock_guard<std::mutex> lk(mu_);
    if (clients_.size() >= cfg_.max_clients) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
      const char kBody[] = "max sse clients reached";
      httpd_resp_send(req, kBody, sizeof(kBody) - 1);
      return ESP_FAIL;
    }
  }

  // Headers must be set *before* httpd_req_async_handler_begin because
  // they're tied to the underlying socket, not the async req copy.
  // Cache-Control: no-cache is defensive against intermediaries that
  // otherwise try to buffer SSE; X-Accel-Buffering does the same for
  // nginx-style reverse proxies.
  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  httpd_req_t* async = nullptr;
  const esp_err_t err = httpd_req_async_handler_begin(req, &async);
  if (err != ESP_OK || async == nullptr) {
    ESP_LOGE(kTag, "async_handler_begin failed: %s", esp_err_to_name(err));
    return err != ESP_OK ? err : ESP_FAIL;
  }

  // Append to client list. The welcome frame goes out synchronously
  // so a reader that never sees a status change still gets a hello
  // (matches the old AsyncEventSource onConnect() behaviour).
  Client new_client{};
  new_client.req = async;
  new_client.last_heartbeat_us = esp_timer_get_time();

  {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.push_back(new_client);
    const std::string welcome = MakeSseFrame(kWelcomeEvent, kWelcomePayload);
    if (!SendFrameLocked(clients_.back(), welcome)) {
      // Welcome failed — client already gone. Roll back.
      httpd_req_async_handler_complete(clients_.back().req);
      clients_.pop_back();
      return ESP_OK;
    }
  }

  // Spin up the heartbeat task on first connect. Subsequent connects
  // reuse the existing task — it walks `clients_` under the mutex.
  // Heartbeat is non-load-bearing — broadcast still works without it,
  // clients just won't get keep-alive pings. Log on failure so the
  // dropped task is visible in the field, and leave heartbeat_task_
  // nullptr so the next connect retries the spawn.
  if (heartbeat_task_ == nullptr) {
    if (xTaskCreate(&SseServer::HeartbeatTaskTrampoline, "sse_hb", 3072, this,
                    tskIDLE_PRIORITY + 1, &heartbeat_task_) != pdPASS) {
      ESP_LOGE(kTag, "heartbeat xTaskCreate failed; SSE keep-alive disabled");
      heartbeat_task_ = nullptr;
    }
  }

  ESP_LOGI(kTag, "client connected, total=%u",
           static_cast<unsigned>(ClientCount()));

  // Returning ESP_OK here relinquishes the sync URI handler's context
  // but the async copy keeps the socket alive. Broadcast + heartbeat
  // threads own the lifecycle from here; the socket is only freed when
  // a send fails (client went away) or the server is destroyed.
  return ESP_OK;
}

bool SseServer::SendFrameLocked(Client& c, std::string_view frame) {
  const esp_err_t rc = httpd_resp_send_chunk(
      c.req, frame.data(), static_cast<ssize_t>(frame.size()));
  if (rc == ESP_OK) {
    c.last_heartbeat_us = esp_timer_get_time();
    return true;
  }
  // Any error means the client is gone — typical return is
  // ESP_ERR_INVALID_STATE on a closed socket, but the underlying
  // lwip call can surface other codes too. Treat all as terminal.
  ESP_LOGI(kTag, "chunk send failed (%s), evicting client",
           esp_err_to_name(rc));
  return false;
}

void SseServer::EvictLocked(size_t idx) {
  if (idx >= clients_.size()) return;
  if (clients_[idx].req) httpd_req_async_handler_complete(clients_[idx].req);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(idx));
}

size_t SseServer::Broadcast(std::string_view event_name,
                            std::string_view data) {
  const std::string frame = MakeSseFrame(event_name, data);
  size_t sent = 0;
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t i = 0; i < clients_.size();) {
    if (SendFrameLocked(clients_[i], frame)) {
      ++sent;
      ++i;
    } else {
      EvictLocked(i);
      // don't increment i — the erase shifted the next element down
    }
  }
  return sent;
}

void SseServer::HeartbeatTaskTrampoline(void* arg) {
  static_cast<SseServer*>(arg)->HeartbeatLoop();
  vTaskDelete(nullptr);
}

void SseServer::HeartbeatLoop() {
  // Fixed comment frame reused across ticks — cheap to send, tiny.
  const std::string ping = MakeSseComment("ping");
  const TickType_t interval = pdMS_TO_TICKS(cfg_.heartbeat_ms);
  while (!shutdown_) {
    // Wait the full interval; block indefinitely on notify so the
    // destructor can shortcut the shutdown path. A spurious notify
    // just re-checks the loop condition, which is benign.
    ulTaskNotifyTake(pdTRUE, interval);
    if (shutdown_) break;

    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < clients_.size();) {
      if (SendFrameLocked(clients_[i], ping)) {
        ++i;
      } else {
        EvictLocked(i);
      }
    }
  }
  heartbeat_task_ = nullptr;
}

}  // namespace btclock
