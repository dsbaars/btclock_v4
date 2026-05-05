#include "proxy_socket_io.hpp"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"

namespace btclock {
namespace proxy {
namespace internal {

namespace {
constexpr const char* kTag = "proxy_socket_io";

void SetTimeouts(int fd, int timeout_ms) {
  if (timeout_ms <= 0) return;
  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
}  // namespace

int OpenTcpSocket(const char* host, uint16_t port, int timeout_ms) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  int rc = getaddrinfo(host, nullptr, &hints, &res);
  if (rc != 0 || !res) {
    ESP_LOGW(kTag, "getaddrinfo(%s) failed: %d", host, rc);
    return -1;
  }
  int fd = -1;
  for (auto* p = res; p; p = p->ai_next) {
    // We only ship over IPv4 today — lwIP's IPv6 stack is gated behind
    // LWIP_IPV6 which this project doesn't enable, and even with it on
    // none of our upstream proxies advertise AAAA records the device
    // would reach. Skip non-INET families rather than #ifdef'ing the
    // ipv6 path; getaddrinfo without AI_ADDRCONFIG can still hand back
    // a v6 record that we'd be unable to bind.
    if (p->ai_family != AF_INET) continue;
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    SetTimeouts(fd, timeout_ms);
    reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_port = htons(port);
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      // Disable Nagle so handshake bytes ship immediately rather than
      // waiting for the kernel's tiny-write coalescer.
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      freeaddrinfo(res);
      return fd;
    }
    ESP_LOGW(kTag, "connect(%s:%u) failed: %d", host, port, errno);
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return -1;
}

int SendAll(int fd, const uint8_t* data, size_t len, int /*timeout_ms*/) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n <= 0) {
      if (errno == EINTR) continue;
      ESP_LOGW(kTag, "send failed: %d", errno);
      return -1;
    }
    sent += static_cast<size_t>(n);
  }
  return 0;
}

int RecvAll(int fd, uint8_t* buf, size_t want, int /*timeout_ms*/) {
  size_t got = 0;
  while (got < want) {
    ssize_t n = recv(fd, buf + got, want - got, 0);
    if (n == 0) {
      ESP_LOGW(kTag, "peer closed during recv (got %u/%u)",
               static_cast<unsigned>(got), static_cast<unsigned>(want));
      return -1;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      ESP_LOGW(kTag, "recv failed: %d (got %u/%u)", errno,
               static_cast<unsigned>(got), static_cast<unsigned>(want));
      return -1;
    }
    got += static_cast<size_t>(n);
  }
  return 0;
}

int RecvUntilParsed(int fd, uint8_t* buf, size_t cap,
                     int (*parse)(const uint8_t*, size_t, size_t*),
                     size_t* out_consumed, int /*timeout_ms*/) {
  size_t have = 0;
  while (have < cap) {
    ssize_t n = recv(fd, buf + have, cap - have, 0);
    if (n <= 0) {
      ESP_LOGW(kTag, "recv failed/EOF mid-parse: %d (have=%u)", errno,
               static_cast<unsigned>(have));
      return -1;
    }
    have += static_cast<size_t>(n);
    int status = parse(buf, have, out_consumed);
    if (status == -1) continue;
    return status;
  }
  ESP_LOGW(kTag, "parse over-budget (cap=%u)", static_cast<unsigned>(cap));
  return -1;
}

}  // namespace internal
}  // namespace proxy
}  // namespace btclock
