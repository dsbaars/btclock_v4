// Host-only parser test for the Ocean DataSource.
//
// Fixture copied from a real api.ocean.xyz/v1/statsnap/<user> response
// captured via curl on 2026-03-14. Only the fields the parser reads
// are kept; values edited to avoid leaking the test account's address
// but the numeric shape (large integer hashrate, tiny BTC earn-per-
// block) matches what Ocean really sends.

#include <string>

#include "doctest.h"
#include "mining_pool_ocean/ocean_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::ocean::parse;

TEST_CASE("ocean::parse — string hashrate, numeric earn") {
  // hashrate_300s as a string (the shape ocean.xyz currently sends);
  // estimated_earn_next_block = 0.00000250 BTC -> 250 sats.
  constexpr const char* body = R"({
    "result": {
      "hashrate_300s": "123456000000000",
      "estimated_earn_next_block": 0.0000025
    }
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "123456000000000");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 250);
}

TEST_CASE("ocean::parse — string earn (current ocean.xyz shape)") {
  // ocean.xyz now quotes estimated_earn_next_block as a string. This is
  // the exact shape returned for 38Qkkei3SuF1Eo45BaYmRHUneRD54yyTFy:
  // "0.00015180" BTC -> 15180 sats.
  constexpr const char* body = R"({
    "result": {
      "hashrate_300s": "1407374883553280",
      "estimated_earn_next_block": "0.00015180"
    }
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "1407374883553280");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 15180);
}

TEST_CASE("ocean::parse — numeric hashrate => stringified") {
  // Defensive path: if the server ever sends the hashrate as a number.
  constexpr const char* body = R"({
    "result": {
      "hashrate_300s": 98765432100,
      "estimated_earn_next_block": 0.0
    }
  })";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "98765432100");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 0);
}

TEST_CASE("ocean::parse — null result => no sample") {
  ParsedStats out;
  CHECK(parse(R"({"result": null})", out));
  CHECK(out.hashrate.empty());
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("ocean::parse — missing hashrate_300s => fail") {
  ParsedStats out;
  CHECK_FALSE(parse(R"({"result": {"foo": 1}})", out));
}

TEST_CASE("ocean::parse — malformed JSON => fail") {
  ParsedStats out;
  CHECK_FALSE(parse("not json", out));
}

}  // namespace
