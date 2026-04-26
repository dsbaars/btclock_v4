#include <cstdint>
#include <set>
#include <string>

#include "doctest.h"
#include "net_util.hpp"

TEST_CASE("FormatApSsid emits BTClock-XXXX from last two MAC bytes") {
  const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAB, 0xCD};
  CHECK(btclock::FormatApSsid(mac) == "BTClock-ABCD");
}

TEST_CASE("FormatApSsid is uppercase hex, zero-padded") {
  // Low bytes should still produce a 4-character suffix with zero padding.
  const uint8_t mac[6] = {0, 0, 0, 0, 0x01, 0x02};
  CHECK(btclock::FormatApSsid(mac) == "BTClock-0102");
}

TEST_CASE("kApPasswordCharset excludes visually ambiguous glyphs") {
  const std::string chars = btclock::kApPasswordCharset;
  // 26 + 26 + 10 minus 7 ambiguous glyphs (I, O, 0, 1, i, l, o) = 55.
  CHECK(chars.size() == 55);
  for (char c : {'0', '1', 'I', 'O', 'l', 'o', 'i'}) {
    CAPTURE(c);
    CHECK(chars.find(c) == std::string::npos);
  }
}

TEST_CASE("GenerateApPassword produces the requested length") {
  uint32_t counter = 0;
  auto rng = [&] { return counter++; };
  CHECK(btclock::GenerateApPassword(rng, 8).size() == 8);
  CHECK(btclock::GenerateApPassword(rng, 16).size() == 16);
  CHECK(btclock::GenerateApPassword(rng, 1).size() == 1);
}

TEST_CASE("GenerateApPassword draws only from the allowed charset") {
  // Exhaustive walk: ask for a very long password, check every byte.
  uint32_t counter = 0;
  const std::string pw =
      btclock::GenerateApPassword([&] { return counter++; }, 1024);
  const std::set<char> allowed(
      btclock::kApPasswordCharset,
      btclock::kApPasswordCharset + sizeof(btclock::kApPasswordCharset) - 1);
  for (char c : pw) {
    CAPTURE(c);
    CHECK(allowed.count(c) == 1);
  }
}

TEST_CASE("GenerateApPassword is deterministic for a given RNG") {
  auto rng1 = [n = uint32_t{42}]() mutable { return n++; };
  auto rng2 = [n = uint32_t{42}]() mutable { return n++; };
  CHECK(btclock::GenerateApPassword(rng1, 12) ==
        btclock::GenerateApPassword(rng2, 12));
}

TEST_CASE("FormatWifiQr emits the standard Wi-Fi join format") {
  CHECK(btclock::FormatWifiQr("BTClock-ABCD", "hunter22") ==
        "WIFI:T:WPA;S:BTClock-ABCD;P:hunter22;;");
}

TEST_CASE("FormatWifiQr handles empty ssid / password without crashing") {
  // The firmware never calls it with empty args, but the helper should
  // still produce a well-formed-looking string rather than UB.
  CHECK(btclock::FormatWifiQr("", "") == "WIFI:T:WPA;S:;P:;;");
}
