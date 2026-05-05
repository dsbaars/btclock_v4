#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace btclock {
namespace proxy {
namespace framing {

// All `Build*` helpers return the exact bytes to write on the wire.
// All `Parse*` helpers take a buffer + length and return a status code,
// optionally reporting how many bytes they consumed so callers can tell
// the streaming framing parser how to slide its window.

// ---- SOCKS5 (RFC 1928) ----------------------------------------------

enum class Socks5Method : uint8_t {
  kNoAuth = 0x00,
  kUserPass = 0x02,
  kRejected = 0xFF,
};

// Greeting offers either {kNoAuth} (when offer_user_pass=false) or
// {kNoAuth, kUserPass} so the server can pick. We always include
// kNoAuth even when auth is configured — many proxies are happy to
// accept anonymous and there's no harm in offering both.
std::vector<uint8_t> BuildSocks5Greeting(bool offer_user_pass);

// Parses the 2-byte server reply. Returns false on a malformed/short
// buffer or version mismatch.
bool ParseSocks5GreetingReply(const uint8_t* buf, size_t len,
                              Socks5Method* out_method);

// User/pass sub-negotiation (RFC 1929). Lengths > 255 get clamped at
// the byte boundary the protocol enforces; callers should validate
// before calling.
std::vector<uint8_t> BuildSocks5UserPass(std::string_view user,
                                         std::string_view pass);

// Returns true when the server returned STATUS=0x00 (success).
bool ParseSocks5UserPassReply(const uint8_t* buf, size_t len);

// CONNECT request with ATYP=domain, so the proxy resolves the host.
// We could fall back to ATYP=ipv4 when host is a literal IP; not
// today — leaving DNS to the proxy is the safer default and matches
// HTTP CONNECT / SOCKS4a behaviour.
std::vector<uint8_t> BuildSocks5Connect(std::string_view host, uint16_t port);

// Reply has variable length (4 + ATYP-dependent address + 2). Returns:
//   >= 0   REP byte (0 = success, others = SOCKS5 failure codes)
//   -1     reply still incomplete
//   -2     malformed (version mismatch)
// On success, writes the total reply byte count to *out_consumed so the
// caller's stream parser can advance.
int ParseSocks5ConnectReply(const uint8_t* buf, size_t len,
                            size_t* out_consumed);

// ---- SOCKS4 / SOCKS4a -----------------------------------------------

// SOCKS4a request format: same 8-byte header as SOCKS4 except the IP
// is replaced with 0.0.0.X (X != 0) to signal "domain follows", then a
// null-terminated user_id, then a null-terminated host. user_id may be
// empty (just a null byte). For plain SOCKS4 (no remote DNS), build
// the request with the resolved IP and skip the trailing host — that
// path uses the IDF's existing esp_transport_socks_proxy_init and
// doesn't need a builder here.
std::vector<uint8_t> BuildSocks4aRequest(std::string_view host, uint16_t port,
                                         std::string_view user_id);

// 8-byte fixed reply. Returns 0x5A on success, the error byte
// (0x5B-0x5D) on protocol-level failure, or -1 on malformed.
int ParseSocks4Reply(const uint8_t* buf, size_t len);

// ---- HTTP CONNECT (RFC 7231 §4.3.6) ---------------------------------

// `basic_auth_b64` is the base64'd "user:pass" string, or empty for
// unauthenticated. We don't insist on a User-Agent: header — most
// proxies don't care for CONNECT, and the WS/HTTP layers add their
// own UA on the eventual GET/UPGRADE.
std::vector<uint8_t> BuildHttpConnectRequest(std::string_view host,
                                             uint16_t port,
                                             std::string_view basic_auth_b64);

// Returns:
//   >= 100  HTTP status code (200 = success, 407 = auth required, …)
//   -1      header block still incomplete (need more bytes)
//   -2      malformed status line
// On success, writes the offset just past `\r\n\r\n` to *out_consumed.
int ParseHttpConnectStatus(const uint8_t* buf, size_t len,
                           size_t* out_consumed);

// Standard base64 (RFC 4648). Used to encode `user:pass` for the
// Proxy-Authorization header. Not exposed beyond this layer; lives in
// the framing TU because the encoder is pure data and host-testable.
std::string Base64Encode(std::string_view in);

}  // namespace framing
}  // namespace proxy
}  // namespace btclock
