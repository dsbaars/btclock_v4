// Host-only parser test for Gobrrr Pool.
//
// Gobrrr runs public-pool under its own domain, so it reuses the same
// parser. This file adds a minimal case documenting that dependency
// — if the public_pool parser ever changes shape, this test will
// fail explicitly at the gobrrr boundary instead of silently reusing
// the new behaviour.

#include <string>

#include "doctest.h"
#include "mining_pool_public_pool/public_pool_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::public_pool::parse;

TEST_CASE("gobrrr: two-worker response via shared public_pool parser") {
  // Sample based on the Gobrrr API (identical shape to
  // public-pool.io). Values chosen so the sum hits a round figure.
  constexpr const char* body = R"({
    "workers": [
      {"hashRate": 600000000000},
      {"hashRate": 400000000000}
    ]
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "1000000000000");
  CHECK(out.workers == 2);
}

TEST_CASE("gobrrr: no workers online => empty hashrate") {
  ParsedStats out;
  CHECK(parse(R"({"workers": [{"hashRate": 0}]})", out));
  CHECK(out.hashrate.empty());
}

}  // namespace
