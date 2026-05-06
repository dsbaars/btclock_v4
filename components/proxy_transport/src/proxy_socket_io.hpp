// Helpers that drive the proxy handshake on a raw lwIP socket fd, so
// the same code path serves both the plain-TCP and TLS-on-top builds.
// Lives in src/ (not include/) because it pulls in lwIP headers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace btclock {
namespace proxy {
namespace internal {

// Open a TCP socket to `host:port`. Returns the fd on success or -1
// (errno set). Resolves via getaddrinfo so it works for hostnames or
// literal IPs. Blocking — meant for use from worker tasks where the
// upper layer (HTTP/WS client) already runs its connect on a worker.
int OpenTcpSocket(const char* host, uint16_t port, int timeout_ms);

// Send all bytes or fail. Loops over send() because lwIP can return
// short writes under memory pressure. Returns 0 on success, -1 on err.
int SendAll(int fd, const uint8_t* data, size_t len, int timeout_ms);

// Receive `want` bytes or fail. Loops similarly. Returns 0 on success,
// -1 on error/timeout/EOF.
int RecvAll(int fd, uint8_t* buf, size_t want, int timeout_ms);

// Receive bytes until the predicate flips from "incomplete" to a
// concrete answer, or until kMaxBytes is exceeded. Used for HTTP
// CONNECT where the reply length depends on header content.
//
// `parse(buf, len, &consumed) -> int` returns:
//   -1  more bytes needed
//   -2  malformed / hard fail
//  >=0  done, with `consumed` bytes used
//
// Returns the parser's terminal status, or -1 on socket / over-budget
// failure.
int RecvUntilParsed(int fd, uint8_t* buf, size_t cap,
                    int (*parse)(const uint8_t*, size_t, size_t*),
                    size_t* out_consumed, int timeout_ms);

}  // namespace internal
}  // namespace proxy
}  // namespace btclock
