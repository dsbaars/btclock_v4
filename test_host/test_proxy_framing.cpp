#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "proxy_transport/proxy_framing.hpp"
#include "proxy_transport/proxy_url.hpp"

using btclock::proxy::framing::BuildHttpConnectRequest;
using btclock::proxy::framing::BuildSocks4aRequest;
using btclock::proxy::framing::BuildSocks5Connect;
using btclock::proxy::framing::BuildSocks5Greeting;
using btclock::proxy::framing::BuildSocks5UserPass;
using btclock::proxy::framing::Base64Encode;
using btclock::proxy::framing::ParseHttpConnectStatus;
using btclock::proxy::framing::ParseSocks4Reply;
using btclock::proxy::framing::ParseSocks5ConnectReply;
using btclock::proxy::framing::ParseSocks5GreetingReply;
using btclock::proxy::framing::ParseSocks5UserPassReply;
using btclock::proxy::framing::Socks5Method;

namespace {

std::string Hex(const std::vector<uint8_t>& v) {
  std::string s;
  for (auto b : v) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", b);
    s += buf;
  }
  return s;
}

}  // namespace

// ---- SOCKS5 greeting -------------------------------------------------

TEST_CASE("SOCKS5 greeting offers NoAuth alone when auth is disabled") {
  auto bytes = BuildSocks5Greeting(false);
  CHECK(Hex(bytes) == "050100");
}

TEST_CASE("SOCKS5 greeting offers NoAuth + UserPass when auth is enabled") {
  auto bytes = BuildSocks5Greeting(true);
  // The order matches the spec: server picks the strongest method it
  // recognises; we offer both so a permissive proxy can short-circuit
  // the sub-negotiation when creds happen to be set but unneeded.
  CHECK(Hex(bytes) == "05020002");
}

TEST_CASE("SOCKS5 greeting reply parser accepts NoAuth and UserPass") {
  Socks5Method m = Socks5Method::kRejected;
  uint8_t ok_no_auth[] = {0x05, 0x00};
  CHECK(ParseSocks5GreetingReply(ok_no_auth, sizeof(ok_no_auth), &m));
  CHECK(m == Socks5Method::kNoAuth);

  uint8_t ok_user_pass[] = {0x05, 0x02};
  CHECK(ParseSocks5GreetingReply(ok_user_pass, sizeof(ok_user_pass), &m));
  CHECK(m == Socks5Method::kUserPass);

  uint8_t reject[] = {0x05, 0xFF};
  CHECK(ParseSocks5GreetingReply(reject, sizeof(reject), &m));
  CHECK(m == Socks5Method::kRejected);
}

TEST_CASE("SOCKS5 greeting reply rejects wrong version + truncation") {
  Socks5Method m{};
  uint8_t v4[] = {0x04, 0x00};
  CHECK(!ParseSocks5GreetingReply(v4, 2, &m));
  uint8_t one[] = {0x05};
  CHECK(!ParseSocks5GreetingReply(one, 1, &m));
}

// ---- SOCKS5 user/pass ------------------------------------------------

TEST_CASE("SOCKS5 user/pass framing matches RFC 1929 byte-for-byte") {
  auto bytes = BuildSocks5UserPass("alice", "hunter2");
  // 01 ULEN U... PLEN P...
  CHECK(Hex(bytes) ==
        "010561"      // 01 05 'a'
        "6c6963"      //       'l' 'i' 'c'
        "6507"        //       'e' (PLEN=7)
        "68756e"      // 'h' 'u' 'n'
        "746572"      // 't' 'e' 'r'
        "32");        // '2'
}

TEST_CASE("SOCKS5 user/pass reply parser") {
  uint8_t ok[] = {0x01, 0x00};
  CHECK(ParseSocks5UserPassReply(ok, 2));
  uint8_t bad_status[] = {0x01, 0x01};
  CHECK(!ParseSocks5UserPassReply(bad_status, 2));
  uint8_t bad_ver[] = {0x05, 0x00};
  CHECK(!ParseSocks5UserPassReply(bad_ver, 2));
  uint8_t one[] = {0x01};
  CHECK(!ParseSocks5UserPassReply(one, 1));
}

// ---- SOCKS5 CONNECT --------------------------------------------------

TEST_CASE("SOCKS5 CONNECT request uses ATYP=domain so proxy resolves DNS") {
  auto bytes = BuildSocks5Connect("api.coinbase.com", 443);
  // 05 01 00 03 LEN 'a''p''i''.''c''o''i''n''b''a''s''e''.''c''o''m' 01 BB
  CHECK(bytes[0] == 0x05);
  CHECK(bytes[1] == 0x01);
  CHECK(bytes[2] == 0x00);
  CHECK(bytes[3] == 0x03);
  CHECK(bytes[4] == 0x10);  // 16 bytes for "api.coinbase.com"
  CHECK(std::memcmp(&bytes[5], "api.coinbase.com", 16) == 0);
  CHECK(bytes[21] == 0x01);  // 443 high
  CHECK(bytes[22] == 0xBB);  // 443 low
  CHECK(bytes.size() == 23);
}

TEST_CASE("SOCKS5 CONNECT reply parses each ATYP correctly") {
  size_t consumed = 0;

  uint8_t ipv4_ok[] = {0x05, 0x00, 0x00, 0x01,
                       0x7F, 0x00, 0x00, 0x01,  // 127.0.0.1
                       0x01, 0xBB};
  CHECK(ParseSocks5ConnectReply(ipv4_ok, sizeof(ipv4_ok), &consumed) == 0);
  CHECK(consumed == 10);

  uint8_t domain_ok[] = {0x05, 0x00, 0x00, 0x03, 0x03,
                         'a', 'b', 'c', 0x00, 0x50};
  CHECK(ParseSocks5ConnectReply(domain_ok, sizeof(domain_ok), &consumed) == 0);
  CHECK(consumed == 10);

  // 4 header + 16 IPv6 + 2 port = 22 bytes.
  uint8_t ipv6_ok[22] = {0x05, 0x00, 0x00, 0x04};
  ipv6_ok[20] = 0x01;
  ipv6_ok[21] = 0xBB;
  CHECK(ParseSocks5ConnectReply(ipv6_ok, sizeof(ipv6_ok), &consumed) == 0);
  CHECK(consumed == 22);
}

TEST_CASE("SOCKS5 CONNECT reply propagates REP byte and incompleteness") {
  size_t consumed = 0;
  uint8_t rejected[] = {0x05, 0x05, 0x00, 0x01,
                        0, 0, 0, 0, 0, 0};
  // 0x05 == "Connection refused"
  CHECK(ParseSocks5ConnectReply(rejected, sizeof(rejected), &consumed) == 5);

  uint8_t partial[] = {0x05, 0x00, 0x00, 0x01, 0x7F, 0x00};
  CHECK(ParseSocks5ConnectReply(partial, sizeof(partial), &consumed) == -1);

  uint8_t bad_ver[] = {0x04, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
  CHECK(ParseSocks5ConnectReply(bad_ver, sizeof(bad_ver), &consumed) == -2);
}

// ---- SOCKS4a ---------------------------------------------------------

TEST_CASE("SOCKS4a request uses 0.0.0.X marker + null-terminated host") {
  auto bytes = BuildSocks4aRequest("api.coinbase.com", 443, "");
  CHECK(bytes[0] == 0x04);
  CHECK(bytes[1] == 0x01);
  CHECK(bytes[2] == 0x01);  // port high
  CHECK(bytes[3] == 0xBB);  // port low
  CHECK(bytes[4] == 0x00);
  CHECK(bytes[5] == 0x00);
  CHECK(bytes[6] == 0x00);
  CHECK(bytes[7] == 0x01);  // 0.0.0.1 — SOCKS4a marker
  CHECK(bytes[8] == 0x00);  // empty user-id terminator
  CHECK(std::memcmp(&bytes[9], "api.coinbase.com", 16) == 0);
  CHECK(bytes[25] == 0x00);  // host terminator
  CHECK(bytes.size() == 26);
}

TEST_CASE("SOCKS4 reply parsing — success and failure") {
  uint8_t ok[] = {0x00, 0x5A, 0, 0, 0, 0, 0, 0};
  CHECK(ParseSocks4Reply(ok, sizeof(ok)) == 0x5A);
  uint8_t reject[] = {0x00, 0x5B, 0, 0, 0, 0, 0, 0};
  CHECK(ParseSocks4Reply(reject, sizeof(reject)) == 0x5B);
  uint8_t bad_first[] = {0x04, 0x5A, 0, 0, 0, 0, 0, 0};
  CHECK(ParseSocks4Reply(bad_first, sizeof(bad_first)) == -1);
}

// ---- HTTP CONNECT ----------------------------------------------------

TEST_CASE("HTTP CONNECT request without auth") {
  auto bytes = BuildHttpConnectRequest("api.coinbase.com", 443, "");
  std::string s(bytes.begin(), bytes.end());
  CHECK(s ==
        "CONNECT api.coinbase.com:443 HTTP/1.1\r\n"
        "Host: api.coinbase.com:443\r\n"
        "\r\n");
}

TEST_CASE("HTTP CONNECT request with Basic auth") {
  // testuser:testpass -> dGVzdHVzZXI6dGVzdHBhc3M=
  auto auth = Base64Encode("testuser:testpass");
  CHECK(auth == "dGVzdHVzZXI6dGVzdHBhc3M=");
  auto bytes = BuildHttpConnectRequest("example.com", 8443, auth);
  std::string s(bytes.begin(), bytes.end());
  CHECK(s ==
        "CONNECT example.com:8443 HTTP/1.1\r\n"
        "Host: example.com:8443\r\n"
        "Proxy-Authorization: Basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n"
        "\r\n");
}

TEST_CASE("HTTP CONNECT status parser") {
  size_t consumed = 0;
  const char* ok =
      "HTTP/1.1 200 Connection established\r\n"
      "Server: 3proxy/0.9\r\n\r\n";
  CHECK(ParseHttpConnectStatus(reinterpret_cast<const uint8_t*>(ok),
                               std::strlen(ok), &consumed) == 200);
  CHECK(consumed == std::strlen(ok));

  const char* auth_required = "HTTP/1.0 407 Proxy Auth Required\r\n\r\n";
  CHECK(ParseHttpConnectStatus(reinterpret_cast<const uint8_t*>(auth_required),
                               std::strlen(auth_required), &consumed) == 407);

  const char* bad_gw = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
  CHECK(ParseHttpConnectStatus(reinterpret_cast<const uint8_t*>(bad_gw),
                               std::strlen(bad_gw), &consumed) == 502);

  const char* partial = "HTTP/1.1 200 Connection established\r\n";
  CHECK(ParseHttpConnectStatus(reinterpret_cast<const uint8_t*>(partial),
                               std::strlen(partial), &consumed) == -1);

  const char* malformed = "ICAP/1.1 200 OK\r\n\r\n";
  CHECK(ParseHttpConnectStatus(reinterpret_cast<const uint8_t*>(malformed),
                               std::strlen(malformed), &consumed) == -2);
}

TEST_CASE("Base64 encoder covers RFC 4648 padding cases") {
  CHECK(Base64Encode("") == "");
  CHECK(Base64Encode("f") == "Zg==");
  CHECK(Base64Encode("fo") == "Zm8=");
  CHECK(Base64Encode("foo") == "Zm9v");
  CHECK(Base64Encode("foob") == "Zm9vYg==");
  CHECK(Base64Encode("fooba") == "Zm9vYmE=");
  CHECK(Base64Encode("foobar") == "Zm9vYmFy");
}

// ---- URL helpers used by the call-site migration --------------------

TEST_CASE("UrlImpliesTls flags https/wss but not http/ws/empty") {
  using btclock::proxy::UrlImpliesTls;
  CHECK(UrlImpliesTls("https://api.coinbase.com/v2/time"));
  CHECK(UrlImpliesTls("wss://relay.damus.io/"));
  CHECK(!UrlImpliesTls("http://192.168.1.42/api/system/info"));
  CHECK(!UrlImpliesTls("ws://localhost/foo"));
  CHECK(!UrlImpliesTls(""));
  CHECK(!UrlImpliesTls("https"));  // too short for the prefix
}

TEST_CASE("PathFromUri extracts path with default '/' fallback") {
  using btclock::proxy::PathFromUri;
  CHECK(PathFromUri("wss://relay.damus.io/") == "/");
  CHECK(PathFromUri("wss://relay.damus.io") == "/");
  CHECK(PathFromUri("wss://mempool.space/api/v1/ws") == "/api/v1/ws");
  CHECK(PathFromUri("https://api.coinbase.com/v2/time?x=1") == "/v2/time?x=1");
  CHECK(PathFromUri("https://example.com:8443/foo") == "/foo");
  CHECK(PathFromUri("/no-scheme/path") == "/no-scheme/path");
  CHECK(PathFromUri("") == "/");
}
