#include "doctest.h"
#include "proxy_transport/proxy_bypass.hpp"
#include "proxy_transport/proxy_config.hpp"

using btclock::proxy::Config;
using btclock::proxy::Kind;
using btclock::proxy::MatchesGlob;
using btclock::proxy::ShouldBypass;
using btclock::proxy::SplitBypassList;

TEST_CASE(
    "MatchesGlob — literal, leading-star, trailing-star, case insensitive") {
  CHECK(MatchesGlob("foo.local", "foo.local"));
  CHECK(MatchesGlob("FOO.LOCAL", "foo.local"));
  CHECK(!MatchesGlob("foo.local", "bar.local"));

  CHECK(MatchesGlob("*.local", "foo.local"));
  CHECK(MatchesGlob("*.local", "x.y.local"));
  CHECK(!MatchesGlob("*.local", "local"));  // star matches at least the dot
  CHECK(!MatchesGlob("*.local", "myhost"));

  CHECK(MatchesGlob("192.168.*", "192.168.1.4"));
  CHECK(MatchesGlob("192.168.*", "192.168.20.97"));
  CHECK(!MatchesGlob("192.168.*", "10.0.0.1"));

  CHECK(MatchesGlob("*", "anything.example.com"));
  CHECK(!MatchesGlob("", "anything.example.com"));
}

TEST_CASE("ShouldBypass — disabled config bypasses everything") {
  Config c;  // kind = kNone
  CHECK(ShouldBypass(c, "api.coinbase.com"));
}

TEST_CASE("ShouldBypass — enabled config respects bypass list") {
  Config c;
  c.kind = Kind::kSocks5;
  c.host = "10.0.0.1";
  c.port = 1080;
  c.bypass = {"*.local", "192.168.*"};

  CHECK(!ShouldBypass(c, "api.coinbase.com"));
  CHECK(ShouldBypass(c, "btclock.local"));
  CHECK(ShouldBypass(c, "192.168.20.97"));
}

TEST_CASE("ShouldBypass — incomplete config (host empty / port 0) bypasses") {
  Config a;
  a.kind = Kind::kSocks5;
  a.port = 1080;
  CHECK(ShouldBypass(a, "x"));

  Config b;
  b.kind = Kind::kSocks5;
  b.host = "10.0.0.1";
  CHECK(ShouldBypass(b, "x"));
}

TEST_CASE("SplitBypassList trims whitespace and drops empties") {
  std::vector<std::string> out;
  SplitBypassList("*.local, 192.168.*, ,127.0.0.1", &out);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == "*.local");
  CHECK(out[1] == "192.168.*");
  CHECK(out[2] == "127.0.0.1");

  out.clear();
  SplitBypassList("", &out);
  CHECK(out.empty());

  out.clear();
  SplitBypassList(" , ,", &out);
  CHECK(out.empty());
}
