// Host-only test for CKPool. The parser is the shared ckpool-family
// walker (already exercised by the Noderunners tests), so this file
// adds minimal coverage pinning ckpool-specific edge cases — notably
// the "worker is offline" case where solo.ckpool.org returns
// "hashrate1m": "0" for an address that hasn't submitted a share in
// the last minute.

#include <string>

#include "doctest.h"
#include "mining_pool_common/ckpool_family_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::ckpool_family::parse;

TEST_CASE("ckpool: solo miner 1T example") {
  // Sample shape copied from a real solo.ckpool.org/users/<bc1q...>
  // response captured 2026-03-14. Full response also has bestshare,
  // shares, shareacc etc. that the parser ignores.
  constexpr const char* body = R"({
    "hashrate1m": "1.2T",
    "hashrate5m": "1.1T",
    "shares": 12345
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "1200000000000");
}

TEST_CASE("ckpool: worker offline => hashrate1m \"0\" => no sample") {
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "0"})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("ckpool: big mining farm E-unit") {
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "45.7E"})", out));
  CHECK(out.hashrate == "45700000000000000000");
}

}  // namespace
