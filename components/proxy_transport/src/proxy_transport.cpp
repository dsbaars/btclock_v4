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
  Config cfg;  // copy so settings reload after init still works
  bool use_tls = false;
  esp_err_t (*crt_bundle_attach)(void* conf) = nullptr;
  int fd = -1;
  esp_tls_t* tls = nullptr;
  bool bypassed = false;
};

// Read/Write call into PollRead/PollWrite below, so forward-declare.
extern "C" int PollRead(esp_transport_handle_t t, int timeout_ms);
extern "C" int PollWrite(esp_transport_handle_t t, int timeout_ms);

extern "C" int Connect(esp_transport_handle_t t, const char* host, int port,
                       int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  // Bypass is evaluated against whatever destination the upper layer
  // hands us — esp_http_client / esp_websocket_client parse cfg.url /
  // cfg.uri and call us with the host extracted, including after
  // redirects. So the same factory-built transport can serve multiple
  // destinations and bypass works correctly per-connection.
  ctx->bypassed = ShouldBypass(ctx->cfg, host ? host : "");

  // Defensive: when esp_websocket_client reconnects internally and the
  // URI didn't carry an explicit port (the common wss://host/ case),
  // it can call into the parent transport with port=0 expecting the
  // transport's `default_port` to fill it in. We set that in
  // MakeProxyTransport, but also fall back here so a missed call site
  // doesn't manifest as a silent EHOSTUNREACH on every reconnect.
  // Without this guard, every wss:// site would die after its first
  // disconnect and never come back until reboot (observed on Rev B
  // 2026-05-06 — see btclock_v4-apr).
  if (port == 0) {
    port = ctx->use_tls ? 443 : 80;
  }

  if (ctx->bypassed) {
    ctx->fd =
        internal::OpenTcpSocket(host, static_cast<uint16_t>(port), timeout_ms);
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

// Return-code contract for esp_transport read/write callbacks (see
// esp_tcp_transport_err_t in esp_transport.h):
//   > 0  bytes transferred
//     0  timeout — no data this round, caller polls again (NOT an error)
//    -1  connection closed by FIN
//    -2  connection failed
//
// Critical for esp_websocket_client: it treats 0 as "keep polling" but
// any negative value as fatal → tear-down + reconnect. Mapping every
// transient mbedtls condition to -1 (the previous bug) caused the WS
// to reconnect on every idle period > network_timeout_ms, restarting
// the TLS handshake from scratch each time. Symptom on Rev B was
// 50–80 s to first populated panel after reboot. Mirrors the
// transport_ssl.c:ssl_read pattern from the IDF tcp_transport.

extern "C" int Read(esp_transport_handle_t t, char* buf, int len,
                    int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  // Wait for readable — without this, the recv()/mbedtls_ssl_read()
  // below would block / time out on its own and surface a transient
  // EAGAIN that we'd have no way to distinguish from a hard error.
  const int p = PollRead(t, timeout_ms);
  if (p < 0) return -1;
  if (p == 0) return 0;  // timeout — no data this round
  if (ctx->tls) {
    ssize_t n = esp_tls_conn_read(ctx->tls, buf, static_cast<size_t>(len));
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (n == ESP_TLS_ERR_SSL_WANT_READ || errno == EAGAIN) return 0;
    return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
  }
  if (ctx->fd < 0) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
  ssize_t n = recv(ctx->fd, buf, len, 0);
  if (n > 0) return static_cast<int>(n);
  if (n == 0) return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

extern "C" int Write(esp_transport_handle_t t, const char* buf, int len,
                     int timeout_ms) {
  auto* ctx = static_cast<Ctx*>(esp_transport_get_context_data(t));
  // Mirror Read's pattern: poll for writability first so a transient
  // would-block doesn't surface as a fatal error to the WS client.
  const int p = PollWrite(t, timeout_ms);
  if (p < 0) return -1;
  if (p == 0) return 0;
  if (ctx->tls) {
    ssize_t n = esp_tls_conn_write(ctx->tls, buf, static_cast<size_t>(len));
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (n == ESP_TLS_ERR_SSL_WANT_WRITE || errno == EAGAIN) return 0;
    return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
  }
  if (ctx->fd < 0) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
  ssize_t n = send(ctx->fd, buf, len, 0);
  if (n > 0) return static_cast<int>(n);
  if (n == 0) return 0;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
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
  return select(fd + 1, nullptr, &ws, nullptr, timeout_ms < 0 ? nullptr : &tv);
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
  if (ctx->tls)
    esp_tls_conn_destroy(ctx->tls);
  else if (ctx->fd >= 0)
    close(ctx->fd);
  delete ctx;
  return ESP_OK;
}

}  // namespace

esp_transport_handle_t MakeProxyTransport(const Config& cfg,
                                          const TransportParams& params) {
  esp_transport_handle_t t = esp_transport_init();
  if (!t) return nullptr;
  auto* ctx = new (std::nothrow) Ctx{};
  if (!ctx) {
    esp_transport_destroy(t);
    return nullptr;
  }
  ctx->cfg = cfg;
  ctx->use_tls = params.use_tls;
  ctx->crt_bundle_attach = params.crt_bundle_attach;
  esp_transport_set_context_data(t, ctx);
  esp_transport_set_func(t, Connect, Read, Write, Close, PollRead, PollWrite,
                         Destroy);
  // Without a default port, esp_websocket_client passes port=0 to the
  // transport on reconnect when the URI didn't include one. The
  // built-in `esp_transport_ssl_init` path sets this for the WS client
  // (esp_websocket_client.c:587 — WEBSOCKET_SSL_DEFAULT_PORT); when
  // ext_transport replaces that path the user has to set it. Setting
  // it here covers both HTTP and WS sites with one call.
  esp_transport_set_default_port(t, params.use_tls ? 443 : 80);
  return t;
}

}  // namespace proxy
}  // namespace btclock
