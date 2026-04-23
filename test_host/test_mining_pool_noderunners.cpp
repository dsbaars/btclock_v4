// Host-only parser test for Noderunners. The parser is the shared
// ckpool-family walker (hashrate1m -> integer h/s). Sample response
// copied from the old firmware's README (CK/Pool v1 docs) with values
// chosen to exercise P/T/G units.

#include <string>

#include "doctest.h"
#include "mining_pool_common/ckpool_family_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::ckpool_family::parse;

TEST_CASE("ckpool_family::parse — P-unit hashrate") {
  constexpr const char* body = R"({"hashrate1m": "12.3P"})";
  ParsedStats out;
  CHECK(parse(body, out));
  // 12.3 * 10^15 = 12300000000000000
  CHECK(out.hashrate == "12300000000000000");
  CHECK_FALSE(out.has_daily_sats);
  CHECK_FALSE(out.has_workers);
}

TEST_CASE("ckpool_family::parse — T-unit hashrate") {
  constexpr const char* body = R"({"hashrate1m": "500T"})";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "500000000000000");
}

TEST_CASE("ckpool_family::parse — G-unit with decimal") {
  constexpr const char* body = R"({"hashrate1m": "7.5G"})";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "7500000000");
}

TEST_CASE("ckpool_family::parse — \"0\" => empty (no sample)") {
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "0"})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("ckpool_family::parse — missing field => empty (no sample)") {
  ParsedStats out;
  CHECK(parse(R"({"other": "value"})", out));
  CHECK(out.hashrate.empty());
}

TEST_CASE("ckpool_family::parse — malformed JSON => fail") {
  ParsedStats out;
  CHECK_FALSE(parse("not json", out));
}

TEST_CASE("ckpool_family::parse — unknown unit => empty") {
  // "X" is not in the multiplier table; normalise_hashrate returns
  // nullopt, parser leaves hashrate empty so base class skips report.
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "5X"})", out));
  CHECK(out.hashrate.empty());
}

}  // namespace
