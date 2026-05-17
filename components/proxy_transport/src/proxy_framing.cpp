#include "proxy_transport/proxy_framing.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace btclock {
namespace proxy {
namespace framing {

namespace {

void Append(std::vector<uint8_t>* v, std::string_view s) {
  v->insert(v->end(), s.begin(), s.end());
}

void AppendByte(std::vector<uint8_t>* v, uint8_t b) {
  v->push_back(b);
}

}  // namespace

// ---- SOCKS5 ---------------------------------------------------------

std::vector<uint8_t> BuildSocks5Greeting(bool offer_user_pass) {
  std::vector<uint8_t> out;
  out.reserve(4);
  out.push_back(0x05);  // VER
  if (offer_user_pass) {
    out.push_back(0x02);  // NMETHODS
    out.push_back(0x00);  // NoAuth
    out.push_back(0x02);  // User/Pass
  } else {
    out.push_back(0x01);  // NMETHODS
    out.push_back(0x00);  // NoAuth
  }
  return out;
}

bool ParseSocks5GreetingReply(std::span<const uint8_t> buf,
                              Socks5Method* out_method) {
  if (buf.size() < 2 || buf[0] != 0x05) return false;
  *out_method = static_cast<Socks5Method>(buf[1]);
  return true;
}

std::vector<uint8_t> BuildSocks5UserPass(std::string_view user,
                                         std::string_view pass) {
  // RFC 1929 caps both fields at 255 bytes; clamping silently here is
  // intentional. Settings layer enforces the same limit on the input
  // path, so a clamp would only fire on a programmer error.
  const uint8_t ulen = static_cast<uint8_t>(std::min<size_t>(user.size(), 255));
  const uint8_t plen = static_cast<uint8_t>(std::min<size_t>(pass.size(), 255));
  std::vector<uint8_t> out;
  out.reserve(3 + ulen + plen);
  out.push_back(0x01);  // sub-negotiation version
  out.push_back(ulen);
  out.insert(out.end(), user.begin(), user.begin() + ulen);
  out.push_back(plen);
  out.insert(out.end(), pass.begin(), pass.begin() + plen);
  return out;
}

bool ParseSocks5UserPassReply(std::span<const uint8_t> buf) {
  return buf.size() >= 2 && buf[0] == 0x01 && buf[1] == 0x00;
}

std::vector<uint8_t> BuildSocks5Connect(std::string_view host, uint16_t port) {
  const uint8_t hlen = static_cast<uint8_t>(std::min<size_t>(host.size(), 255));
  std::vector<uint8_t> out;
  out.reserve(7 + hlen);
  out.push_back(0x05);  // VER
  out.push_back(0x01);  // CMD = CONNECT
  out.push_back(0x00);  // RSV
  out.push_back(0x03);  // ATYP = domainname
  out.push_back(hlen);
  out.insert(out.end(), host.begin(), host.begin() + hlen);
  out.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(port & 0xFF));
  return out;
}

int ParseSocks5ConnectReply(std::span<const uint8_t> buf,
                            size_t* out_consumed) {
  if (out_consumed) *out_consumed = 0;
  if (buf.size() < 4) return -1;
  if (buf[0] != 0x05) return -2;
  const uint8_t rep = buf[1];
  const uint8_t atyp = buf[3];
  size_t addr_len = 0;
  switch (atyp) {
    case 0x01:
      addr_len = 4;
      break;    // IPv4
    case 0x03:  // domain
      if (buf.size() < 5) return -1;
      addr_len = 1 + buf[4];
      break;
    case 0x04:
      addr_len = 16;
      break;  // IPv6
    default:
      return -2;
  }
  const size_t total = 4 + addr_len + 2;
  if (buf.size() < total) return -1;
  if (out_consumed) *out_consumed = total;
  return rep;
}

// ---- SOCKS4a --------------------------------------------------------

std::vector<uint8_t> BuildSocks4aRequest(std::string_view host, uint16_t port,
                                         std::string_view user_id) {
  const uint8_t ulen =
      static_cast<uint8_t>(std::min<size_t>(user_id.size(), 255));
  std::vector<uint8_t> out;
  out.reserve(9 + ulen + host.size() + 1);
  out.push_back(0x04);  // VN
  out.push_back(0x01);  // CD = CONNECT
  out.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(port & 0xFF));
  // 0.0.0.X (X != 0) signals SOCKS4a "resolve this domain" mode.
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x01);
  out.insert(out.end(), user_id.begin(), user_id.begin() + ulen);
  out.push_back(0x00);  // user-id terminator
  out.insert(out.end(), host.begin(), host.end());
  out.push_back(0x00);  // host terminator
  return out;
}

int ParseSocks4Reply(std::span<const uint8_t> buf) {
  if (buf.size() < 8 || buf[0] != 0x00) return -1;
  return buf[1];
}

// ---- HTTP CONNECT ---------------------------------------------------

std::vector<uint8_t> BuildHttpConnectRequest(std::string_view host,
                                             uint16_t port,
                                             std::string_view basic_auth_b64) {
  // sprintf to a stack buffer for the request line — the host comes
  // straight from settings and is bounded by NVS (string keys cap at
  // 4063 chars), but we cap to a generous 256 anyway to match the SNI
  // sanity limit elsewhere in the firmware.
  char line[512];
  int n = std::snprintf(line, sizeof(line),
                        "CONNECT %.*s:%u HTTP/1.1\r\nHost: %.*s:%u\r\n",
                        static_cast<int>(std::min<size_t>(host.size(), 256)),
                        host.data(), static_cast<unsigned>(port),
                        static_cast<int>(std::min<size_t>(host.size(), 256)),
                        host.data(), static_cast<unsigned>(port));
  std::vector<uint8_t> out;
  if (n <= 0) return out;
  out.reserve(static_cast<size_t>(n) + basic_auth_b64.size() + 32);
  out.insert(out.end(), line, line + n);
  if (!basic_auth_b64.empty()) {
    static const char kHeader[] = "Proxy-Authorization: Basic ";
    out.insert(out.end(), kHeader, kHeader + sizeof(kHeader) - 1);
    out.insert(out.end(), basic_auth_b64.begin(), basic_auth_b64.end());
    out.push_back('\r');
    out.push_back('\n');
  }
  out.push_back('\r');
  out.push_back('\n');
  return out;
}

int ParseHttpConnectStatus(std::span<const uint8_t> buf, size_t* out_consumed) {
  if (out_consumed) *out_consumed = 0;
  const size_t len = buf.size();
  // Need at least "HTTP/1.x ddd \r\n\r\n" — 14 bytes minimum.
  if (len < 14) return -1;
  // Status line must start with HTTP/1.
  if (std::memcmp(buf.data(), "HTTP/1.", 7) != 0) return -2;

  // Parse status code.
  size_t i = 8;  // skip "HTTP/1.x"
  while (i < len && buf[i] == ' ') ++i;
  if (i + 3 > len) return -1;
  if (!std::isdigit(buf[i]) || !std::isdigit(buf[i + 1]) ||
      !std::isdigit(buf[i + 2])) {
    return -2;
  }
  const int status =
      (buf[i] - '0') * 100 + (buf[i + 1] - '0') * 10 + (buf[i + 2] - '0');

  // Find end of headers.
  for (size_t j = i + 3; j + 3 < len; ++j) {
    if (buf[j] == '\r' && buf[j + 1] == '\n' && buf[j + 2] == '\r' &&
        buf[j + 3] == '\n') {
      if (out_consumed) *out_consumed = j + 4;
      return status;
    }
  }
  return -1;
}

std::string Base64Encode(std::string_view in) {
  static const char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8) |
                       static_cast<uint8_t>(in[i + 2]);
    out.push_back(kTbl[(v >> 18) & 0x3F]);
    out.push_back(kTbl[(v >> 12) & 0x3F]);
    out.push_back(kTbl[(v >> 6) & 0x3F]);
    out.push_back(kTbl[v & 0x3F]);
    i += 3;
  }
  if (i < in.size()) {
    uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
    out.push_back(kTbl[(v >> 18) & 0x3F]);
    out.push_back(kTbl[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < in.size()) ? kTbl[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

}  // namespace framing
}  // namespace proxy
}  // namespace btclock
