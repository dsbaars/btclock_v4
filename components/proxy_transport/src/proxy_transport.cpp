// Unified proxy esp_transport. One handle, one fd, one optional TLS
// session. Used directly as
//   esp_http_client_config_t.transport
//   esp_websocket_client_config_t.ext_transport
//
// Why not chain on top of esp_transport_ssl: that transport's
// connect() unconditionally calls esp_tls_plain_tcp_connect, which
// opens its own socket and ignores any parent transport. There is no
// way in IDF v6.0 to chain SSL on top of an arbitrary parent. The
// fix is to drive esp_tls ourselves at the socket level: we open the
// fd, run the proxy handshake on it, then hand the fd to esp_tls via
// esp_tls_set_conn_sockfd + esp_tls_set_conn_state(ESP_TLS_CONNECTING).
// That state skips the built-in tcp_connect and goes straight to
// create_ssl_handle + handshake.

#include "proxy_transport/proxy_transport.hpp"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_transport.h"

#include "proxy_handshake.hpp"
#include "proxy_socket_io.hpp"
#include "proxy_transport/proxy_bypass.hpp"

namespace btclock {
namespace proxy {

namespace {

constexpr const char* kTag = "proxy_transport";

struct Ctx {
  Config cfg;                 // copy so settings reload after init still works
  std::string dest_host;      // upper layer's intended destination
  bool use_tls = false;
  esp_err_t (*crt_bundle_attach)(void* conf) = nullptr;
  int fd = -1;
  esp_tls_t* tls = nullptr;
  bool bypassed = false;
};

extern "C" int Connect(esp_transport_handle_t t, const char* host, int port,
                        int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  // Re-evaluate bypass against the destination we were just given —
  // upper-layer client may pass a different host than the one we
  // captured at MakeProxyTransport time (e.g. http_client follows
  // redirects to a new host).
  ctx->bypassed = ShouldBypass(ctx->cfg, host ? host : "");

  if (ctx->bypassed) {
    ctx->fd = internal::OpenTcpSocket(host, static_cast<uint16_t>(port),
                                       timeout_ms);
    if (ctx->fd < 0) return -1;
  } else {
    ctx->fd = internal::OpenTcpSocket(ctx->cfg.host.c_str(), ctx->cfg.port,
                                       timeout_ms);
    if (ctx->fd < 0) {
      ESP_LOGW(kTag, "proxy connect to %s:%u failed", ctx->cfg.host.c_str(),
               ctx->cfg.port);
      return -1;
    }
    if (internal::RunProxyHandshake(ctx->fd, ctx->cfg, host,
                                     static_cast<uint16_t>(port),
                                     timeout_ms) != 0) {
      close(ctx->fd);
      ctx->fd = -1;
      return -1;
    }
  }

  if (!ctx->use_tls) return 0;

  ctx->tls = esp_tls_init();
  if (!ctx->tls) {
    close(ctx->fd);
    ctx->fd = -1;
    return -1;
  }
  if (esp_tls_set_conn_sockfd(ctx->tls, ctx->fd) != ESP_OK) {
    ESP_LOGE(kTag, "esp_tls_set_conn_sockfd failed");
    esp_tls_conn_destroy(ctx->tls);
    ctx->tls = nullptr;
    ctx->fd = -1;  // destroyed by esp_tls_conn_destroy via socket close
    return -1;
  }
  // ESP_TLS_CONNECTING tells esp_tls_low_level_conn that the TCP
  // socket is already up — it skips tcp_connect, calls
  // create_ssl_handle (which sets up mbedtls and SNI), then runs the
  // handshake. The hostname we pass here is the *real* destination
  // so SNI and CN validation target it, not the proxy.
  if (esp_tls_set_conn_state(ctx->tls, ESP_TLS_CONNECTING) != ESP_OK) {
    esp_tls_conn_destroy(ctx->tls);
    ctx->tls = nullptr;
    ctx->fd = -1;
    return -1;
  }
  esp_tls_cfg_t tls_cfg = {};
  tls_cfg.is_plain_tcp = false;
  tls_cfg.non_block = false;
  tls_cfg.timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;
  tls_cfg.crt_bundle_attach = ctx->crt_bundle_attach;
  int rc = esp_tls_conn_new_sync(host, static_cast<int>(strlen(host)), port,
                                  &tls_cfg, ctx->tls);
  if (rc != 1) {
    ESP_LOGW(kTag, "esp_tls handshake failed (rc=%d) for %s:%d", rc, host,
             port);
    esp_tls_conn_destroy(ctx->tls);
    ctx->tls = nullptr;
    ctx->fd = -1;
    return -1;
  }
  return 0;
}

extern "C" int Read(esp_transport_handle_t t, char* buf, int len,
                    int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  if (ctx->tls) {
    ssize_t n = esp_tls_conn_read(ctx->tls, buf, static_cast<size_t>(len));
    return n < 0 ? -1 : static_cast<int>(n);
  }
  // Plain TCP. Honour timeout via select() before recv() — recv with
  // SO_RCVTIMEO would also work but we already use select in PollRead.
  if (ctx->fd < 0) return -1;
  if (timeout_ms >= 0) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(ctx->fd, &rs);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int s = select(ctx->fd + 1, &rs, nullptr, nullptr, &tv);
    if (s == 0) return 0;
    if (s < 0) return -1;
  }
  ssize_t n = recv(ctx->fd, buf, len, 0);
  return n < 0 ? -1 : static_cast<int>(n);
}

extern "C" int Write(esp_transport_handle_t t, const char* buf, int len,
                     int /*timeout_ms*/) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  if (ctx->tls) {
    ssize_t n = esp_tls_conn_write(ctx->tls, buf, static_cast<size_t>(len));
    return n < 0 ? -1 : static_cast<int>(n);
  }
  if (ctx->fd < 0) return -1;
  ssize_t n = send(ctx->fd, buf, len, 0);
  return n < 0 ? -1 : static_cast<int>(n);
}

extern "C" int PollRead(esp_transport_handle_t t, int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  // For TLS, prefer esp_tls_get_bytes_avail before falling back to
  // select on the underlying sockfd — there can be plaintext queued
  // inside mbedtls that select() can't see.
  int fd = ctx->fd;
  if (ctx->tls) {
    int avail = esp_tls_get_bytes_avail(ctx->tls);
    if (avail > 0) return 1;
    if (esp_tls_get_conn_sockfd(ctx->tls, &fd) != ESP_OK) return -1;
  }
  if (fd < 0) return -1;
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(fd, &rs);
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int s = select(fd + 1, &rs, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
  return s;
}

extern "C" int PollWrite(esp_transport_handle_t t, int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  int fd = ctx->fd;
  if (ctx->tls && esp_tls_get_conn_sockfd(ctx->tls, &fd) != ESP_OK) {
    return -1;
  }
  if (fd < 0) return -1;
  fd_set ws;
  FD_ZERO(&ws);
  FD_SET(fd, &ws);
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  return select(fd + 1, nullptr, &ws, nullptr,
                timeout_ms < 0 ? nullptr : &tv);
}

extern "C" int Close(esp_transport_handle_t t) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  if (ctx->tls) {
    esp_tls_conn_destroy(ctx->tls);
    ctx->tls = nullptr;
    ctx->fd = -1;  // closed by esp_tls_conn_destroy
    return 0;
  }
  if (ctx->fd >= 0) {
    close(ctx->fd);
    ctx->fd = -1;
  }
  return 0;
}

extern "C" int Destroy(esp_transport_handle_t t) {
  if (!t) return ESP_OK;
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  if (!ctx) return ESP_OK;
  if (ctx->tls) esp_tls_conn_destroy(ctx->tls);
  else if (ctx->fd >= 0) close(ctx->fd);
  delete ctx;
  return ESP_OK;
}

}  // namespace

esp_transport_handle_t MakeProxyTransport(const Config& cfg,
                                          const char* dest_host,
                                          const TransportParams& params) {
  esp_transport_handle_t t = esp_transport_init();
  if (!t) return nullptr;
  auto* ctx = new (std::nothrow) Ctx{};
  if (!ctx) {
    esp_transport_destroy(t);
    return nullptr;
  }
  ctx->cfg = cfg;
  if (dest_host) ctx->dest_host = dest_host;
  ctx->use_tls = params.use_tls;
  ctx->crt_bundle_attach = params.crt_bundle_attach;
  esp_transport_set_context_data(t, ctx);
  esp_transport_set_func(t, Connect, Read, Write, Close, PollRead, PollWrite,
                          Destroy);
  return t;
}

}  // namespace proxy
}  // namespace btclock
