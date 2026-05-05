// Pure proxy handshake driver: takes a freshly connected fd to the
// proxy and runs the protocol-specific bytes until the proxy is in
// "data relay" mode. Caller hands the fd off to a TLS layer (or uses
// it raw) afterwards.

#pragma once

#include <cstdint>

#include "proxy_transport/proxy_config.hpp"

namespace btclock {
namespace proxy {
namespace internal {

// Runs the proxy handshake on `fd` for the destination `dest_host:port`.
// Returns 0 on success, -1 on protocol or socket failure (errno-style
// behaviour: caller closes the fd on failure).
int RunProxyHandshake(int fd, const Config& cfg, const char* dest_host,
                      uint16_t dest_port, int timeout_ms);

}  // namespace internal
}  // namespace proxy
}  // namespace btclock
