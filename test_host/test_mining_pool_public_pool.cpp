// Host-only parser test for Public Pool.
//
// Fixture: shape copied from the public-pool.io GitHub docs
//   https://github.com/benjamin-wilson/public-pool
// /api/client/<pubkey> endpoint. We ignore everything outside the
// `workers` array since the parser only needs hashRate-per-worker.

#include <string>

#include "doctest.h"
#include "mining_pool_public_pool/public_pool_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::public_pool::parse;

TEST_CASE("public_pool::parse — sum of three workers") {
  constexpr const char* body = R"({
    "workers": [
      {"hashRate": 500000000000},
      {"hashRate": 300000000000},
      {"hashRate": 200000000000}
    ]
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "1000000000000");
  CHECK(out.has_workers);
  CHECK(out.workers == 3);
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("public_pool::parse — fractional hashRate rounded") {
  // Public-pool stores hashRate as a double in h/s; the server
  // occasionally returns a non-integer (moving average). Our rounding
  // preserves total count and keeps the string integer-valued.
  constexpr const char* body = R"({
    "workers": [
      {"hashRate": 123456789.4},
      {"hashRate": 987654321.6}
    ]
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  // 123456789 + 987654322 = 1111111111
  CHECK(out.hashrate == "1111111111");
  CHECK(out.workers == 2);
}

TEST_CASE("public_pool::parse — empty workers array => no sample") {
  ParsedStats out;
  CHECK(parse(R"({"workers": []})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("public_pool::parse — all-zero workers => no sample") {
  ParsedStats out;
  CHECK(parse(R"({"workers": [{"hashRate": 0}, {"hashRate": 0}]})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("public_pool::parse — missing workers field => no sample") {
  ParsedStats out;
  CHECK(parse(R"({"other": 1})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("public_pool::parse — malformed JSON => fail") {
  ParsedStats out;
  CHECK_FALSE(parse("not json", out));
}

}  // namespace
