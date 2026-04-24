#include "doctest.h"

#include <cstdint>
#include <initializer_list>
#include <string>

#include "net_util/hostname.hpp"

TEST_CASE("ComputeHostname emits the v3_fci 6-hex-char shape") {
  // MAC copied from the bug report (Rev B board 98:88:e0:9d:55:30).
  // Bug was /api/settings returning "btclock-5530" (4 chars) while
  // mDNS published "btclock-9d5530.local" (6 chars). The 6-char form
  // wins — it's what the user can ping.
  const uint8_t mac[6] = {0x98, 0x88, 0xe0, 0x9d, 0x55, 0x30};
  CHECK(btclock::net_util::ComputeHostname("btclock", mac) ==
        "btclock-9d5530");
}

TEST_CASE("ComputeHostname uses lowercase hex, zero-padded") {
  const uint8_t mac[6] = {0, 0, 0, 0x01, 0x02, 0x03};
  CHECK(btclock::net_util::ComputeHostname("btclock", mac) ==
        "btclock-010203");
}

TEST_CASE("ComputeHostname honours custom prefixes") {
  const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAB, 0xCD};
  CHECK(btclock::net_util::ComputeHostname("livingroom", mac) ==
        "livingroom-efabcd");
}

TEST_CASE("ComputeHostname truncates prefixes over the DNS ceiling") {
  // 56 is the max the helper accepts so "-xxxxxx" still fits in the 63-char
  // DNS label limit. A longer prefix must be silently clipped.
  const std::string overlong(80, 'a');
  const uint8_t mac[6] = {0, 0, 0, 0xAB, 0xCD, 0xEF};
  const std::string out =
      btclock::net_util::ComputeHostname(overlong, mac);
  // Prefix gets clipped to 56 chars, plus "-abcdef" (7) = 63.
  CHECK(out.size() == 63);
  CHECK(out.substr(0, 56) == std::string(56, 'a'));
  CHECK(out.substr(56) == "-abcdef");
}

TEST_CASE("ComputeHostname only looks at mac[3..5]") {
  // The first three bytes are part of the OUI — identical for every
  // unit from a given vendor, and useless for distinguishing devices
  // on a LAN. The helper must ignore them.
  const uint8_t mac_a[6] = {0x00, 0x00, 0x00, 0x9d, 0x55, 0x30};
  const uint8_t mac_b[6] = {0xFF, 0xFF, 0xFF, 0x9d, 0x55, 0x30};
  CHECK(btclock::net_util::ComputeHostname("btclock", mac_a) ==
        btclock::net_util::ComputeHostname("btclock", mac_b));
}
