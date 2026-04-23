// Host-only parser test for the Braiins DataSource.
//
// The fixture is a sanitised copy of a real pool.braiins.com response
// captured 2026-03-14 against a throwaway test account, reduced to the
// fields the parser consults. Source documentation is in the old
// firmware test data and the Braiins API docs:
//   https://academy.braiins.com/en/braiins-pool/monitoring/
// Fields outside the `btc` sub-object are stripped; the parser ignores
// them. Values chosen so the expected hashrate exceeds 1 Eh/s and the
// today_reward produces a whole-sats count > 10000.

#include <string>

#include "doctest.h"
#include "mining_pool_braiins/braiins_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::braiins::parse;

TEST_CASE("braiins::parse — typical Th/s response") {
  // hash_rate_5m = 250.5, unit = Th/s  -> expect "251" + 12 zeros
  //                                       = "251000000000000"
  // today_reward = 0.00123456 BTC       -> expect 123456 sats
  constexpr const char* body = R"({
    "btc": {
      "hash_rate_5m": 250.5,
      "hash_rate_unit": "Th/s",
      "today_reward": 0.00123456
    }
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "251000000000000");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 123456);
}

TEST_CASE("braiins::parse — Ph/s response") {
  // 12.3 Ph/s -> rounded 12 + 15 zeros
  constexpr const char* body = R"({
    "btc": {
      "hash_rate_5m": 12.3,
      "hash_rate_unit": "Ph/s",
      "today_reward": 0.0
    }
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "12000000000000000");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 0);
}

TEST_CASE("braiins::parse — null btc sub-object => no sample") {
  // When the account has no BTC activity Braiins returns
  // {"btc": null}. The old firmware treated that as hashrate="0",
  // which the base class filters out as "no sample yet". We match
  // that semantics by returning true + empty hashrate.
  constexpr const char* body = R"({"btc": null})";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate.empty());
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("braiins::parse — unknown unit => fail") {
  constexpr const char* body = R"({
    "btc": {"hash_rate_5m": 1.0, "hash_rate_unit": "Xh/s"}
  })";
  ParsedStats out;
  CHECK_FALSE(parse(body, out));
}

TEST_CASE("braiins::parse — malformed JSON => fail") {
  ParsedStats out;
  CHECK_FALSE(parse("not json", out));
}

TEST_CASE("braiins::parse — missing today_reward leaves daily unset") {
  constexpr const char* body = R"({
    "btc": {"hash_rate_5m": 5.0, "hash_rate_unit": "Gh/s"}
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "5000000000");
  CHECK_FALSE(out.has_daily_sats);
}

}  // namespace
