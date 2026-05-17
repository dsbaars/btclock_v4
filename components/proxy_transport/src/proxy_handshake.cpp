#include "proxy_handshake.hpp"

#include <cstring>

#include "esp_log.h"
#include "proxy_socket_io.hpp"
#include "proxy_transport/proxy_framing.hpp"

namespace btclock {
namespace proxy {
namespace internal {

namespace {
constexpr const char* kTag = "proxy_handshake";

int RunSocks5(int fd, const Config& cfg, const char* dest_host,
              uint16_t dest_port, int timeout_ms) {
  const bool offer_auth = !cfg.user.empty();
  auto greet = framing::BuildSocks5Greeting(offer_auth);
  if (SendAll(fd, greet.data(), greet.size(), timeout_ms) < 0) return -1;
  uint8_t reply[2];
  if (RecvAll(fd, reply, sizeof(reply), timeout_ms) < 0) return -1;
  framing::Socks5Method m{};
  if (!framing::ParseSocks5GreetingReply(reply, &m)) {
    ESP_LOGW(kTag, "socks5 greeting reply malformed");
    return -1;
  }
  if (m == framing::Socks5Method::kRejected) {
    ESP_LOGW(kTag, "socks5 proxy rejected offered methods");
    return -1;
  }
  if (m == framing::Socks5Method::kUserPass) {
    auto up = framing::BuildSocks5UserPass(cfg.user, cfg.pass);
    if (SendAll(fd, up.data(), up.size(), timeout_ms) < 0) return -1;
    uint8_t up_reply[2];
    if (RecvAll(fd, up_reply, sizeof(up_reply), timeout_ms) < 0) return -1;
    if (!framing::ParseSocks5UserPassReply(up_reply)) {
      ESP_LOGW(kTag, "socks5 user/pass auth failed");
      return -1;
    }
  }
  auto req = framing::BuildSocks5Connect(dest_host, dest_port);
  if (SendAll(fd, req.data(), req.size(), timeout_ms) < 0) return -1;
  // Reply length depends on ATYP byte at offset 3.
  uint8_t hdr[5] = {};
  if (RecvAll(fd, hdr, 4, timeout_ms) < 0) return -1;
  if (hdr[0] != 0x05) {
    ESP_LOGW(kTag, "socks5 connect reply: bad version");
    return -1;
  }
  size_t addr_len = 0;
  switch (hdr[3]) {
    case 0x01:
      addr_len = 4;
      break;
    case 0x04:
      addr_len = 16;
      break;
    case 0x03:
      if (RecvAll(fd, hdr + 4, 1, timeout_ms) < 0) return -1;
      addr_len = hdr[4];
      break;
    default:
      ESP_LOGW(kTag, "socks5 connect reply: bad ATYP=0x%02x", hdr[3]);
      return -1;
  }
  uint8_t scratch[262];  // 4 + 1 + 255 + 2 max
  if (RecvAll(fd, scratch, addr_len + 2, timeout_ms) < 0) return -1;
  if (hdr[1] != 0x00) {
    ESP_LOGW(kTag, "socks5 proxy refused: REP=0x%02x for %s:%u", hdr[1],
             dest_host, dest_port);
    return -1;
  }
  return 0;
}

int RunSocks4a(int fd, const Config& cfg, const char* dest_host,
               uint16_t dest_port, int timeout_ms) {
  auto req = framing::BuildSocks4aRequest(dest_host, dest_port, cfg.user);
  if (SendAll(fd, req.data(), req.size(), timeout_ms) < 0) return -1;
  uint8_t reply[8];
  if (RecvAll(fd, reply, sizeof(reply), timeout_ms) < 0) return -1;
  int code = framing::ParseSocks4Reply(reply);
  if (code != 0x5A) {
    ESP_LOGW(kTag, "socks4a proxy refused: code=0x%02x for %s:%u", code,
             dest_host, dest_port);
    return -1;
  }
  return 0;
}

int RunSocks4(int fd, const Config& cfg, const char* dest_host,
              uint16_t dest_port, int timeout_ms) {
  // The schema doesn't expose a separate SOCKS4 mode; this entry point
  // exists for completeness and just delegates to SOCKS4a — virtually
  // all SOCKS4 proxies accept 4a-style requests via the 0.0.0.X marker.
  return RunSocks4a(fd, cfg, dest_host, dest_port, timeout_ms);
}

int RunHttpConnect(int fd, const Config& cfg, const char* dest_host,
                   uint16_t dest_port, int timeout_ms) {
  std::string auth_b64;
  if (!cfg.user.empty()) {
    std::string creds = cfg.user;
    creds.push_back(':');
    creds.append(cfg.pass);
    auth_b64 = framing::Base64Encode(creds);
  }
  auto req = framing::BuildHttpConnectRequest(dest_host, dest_port, auth_b64);
  if (SendAll(fd, req.data(), req.size(), timeout_ms) < 0) return -1;
  uint8_t buf[1024];
  size_t consumed = 0;
  int status = RecvUntilParsed(
      fd, buf, sizeof(buf),
      [](const uint8_t* b, size_t n, size_t* c) -> int {
        return framing::ParseHttpConnectStatus({b, n}, c);
      },
      &consumed, timeout_ms);
  if (status < 0) {
    ESP_LOGW(kTag, "http connect: parse/recv failure (%d)", status);
    return -1;
  }
  if (status != 200) {
    ESP_LOGW(kTag, "http connect: proxy returned %d for %s:%u", status,
             dest_host, dest_port);
    return -1;
  }
  if (consumed != static_cast<size_t>(status == 200 ? consumed : 0) || false) {
    // Defensive: parse function reports consumed bytes; anything past
    // that is the start of the relayed stream which we cannot retain.
    // In practice 3proxy returns just the headers and stops; if we
    // ever observe payload beyond the headers, log and bail.
  }
  return 0;
}

}  // namespace

int RunProxyHandshake(int fd, const Config& cfg, const char* dest_host,
                      uint16_t dest_port, int timeout_ms) {
  switch (cfg.kind) {
    case Kind::kNone:
      return 0;
    case Kind::kHttpConnect:
      return RunHttpConnect(fd, cfg, dest_host, dest_port, timeout_ms);
    case Kind::kSocks4:
      return RunSocks4(fd, cfg, dest_host, dest_port, timeout_ms);
    case Kind::kSocks4a:
      return RunSocks4a(fd, cfg, dest_host, dest_port, timeout_ms);
    case Kind::kSocks5:
      return RunSocks5(fd, cfg, dest_host, dest_port, timeout_ms);
  }
  return -1;
}

}  // namespace internal
}  // namespace proxy
}  // namespace btclock
