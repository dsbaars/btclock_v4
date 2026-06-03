#include "diag/udp_log.hpp"

#if defined(BTCLOCK_DIAG_UDP_LOG) && BTCLOCK_DIAG_UDP_LOG

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace btclock {
namespace {

int g_sock = -1;
vprintf_like_t g_prev = nullptr;
struct sockaddr_in g_dst{};

// lwip's own logging during sendto() would recurse back into this hook;
// the guard keeps the UDP send one-shot (serial mirror still runs).
thread_local bool g_in_hook = false;

int UdpLogVprintf(const char* fmt, va_list ap) {
  char buf[256];
  va_list ap2;
  va_copy(ap2, ap);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
  va_end(ap2);

  if (g_sock >= 0 && n > 0 && !g_in_hook) {
    g_in_hook = true;
    const int len = n < static_cast<int>(sizeof(buf))
                        ? n
                        : static_cast<int>(sizeof(buf)) - 1;
    sendto(g_sock, buf, static_cast<size_t>(len), 0,
           reinterpret_cast<struct sockaddr*>(&g_dst), sizeof(g_dst));
    g_in_hook = false;
  }

  // Preserve serial output so nothing is lost if a TTY is ever attached.
  return g_prev ? g_prev(fmt, ap) : n;
}

}  // namespace

void InstallUdpLogSink(uint16_t port) {
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock < 0) return;
  int on = 1;
  setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
  // Non-blocking: never stall the esp_log lock on a full TX buffer.
  const int flags = fcntl(g_sock, F_GETFL, 0);
  fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);

  g_dst.sin_family = AF_INET;
  g_dst.sin_port = htons(port);
  g_dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);  // 255.255.255.255

  g_prev = esp_log_set_vprintf(&UdpLogVprintf);
}

}  // namespace btclock

#else  // BTCLOCK_DIAG_UDP_LOG disabled — no-op stub (no socket, no lwip).

namespace btclock {
void InstallUdpLogSink(uint16_t /*port*/) {}
}  // namespace btclock

#endif
