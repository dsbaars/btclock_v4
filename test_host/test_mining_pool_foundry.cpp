// Host-only parser test for the Foundry DataSource.
//
// Fixture shape mirrors the v2 earnings response documented in the
// Foundry OpenAPI spec at api.foundryusapool.com/openapi-spec.json
// (operationId getEarningsV2). Each entry is one UTC day's aggregate;
// the parser walks to the latest entry and surfaces its hashrate +
// totalAmount.

#include <string>

#include "doctest.h"
#include "mining_pool_foundry/foundry_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::foundry::parse;

TEST_CASE("foundry::parse — v2 earnings, latest entry wins") {
  // Two-day window. Latest entry = current UTC day partial.
  // hashrate = 12_345_678_901_234 H/s
  // totalAmount = 0.00056789 BTC -> 56789 sats
  constexpr const char* body = R"([
    {"startTime":"2024-01-01T00:00:00.000+00:00",
     "endTime":"2024-01-02T00:00:00.000+00:00",
     "totalAmount":0.00012345,
     "hashrate":11111111111111,
     "ppsBaseAmount":0.0,"txFeeRewardAmount":0.0,
     "fppsRatePercent":100.0,"poolFeeAmount":0.0,
     "feeRatePercent":0.0,"grossEarnings":0.00012345},
    {"startTime":"2024-01-02T00:00:00.000+00:00",
     "endTime":"2024-01-03T00:00:00.000+00:00",
     "totalAmount":0.00056789,
     "hashrate":12345678901234,
     "ppsBaseAmount":0.0,"txFeeRewardAmount":0.0,
     "fppsRatePercent":100.0,"poolFeeAmount":0.0,
     "feeRatePercent":0.0,"grossEarnings":0.00056789}
  ])";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "12345678901234");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 56789);
}

TEST_CASE("foundry::parse — totalAmount missing leaves daily unset") {
  // Spec marks totalAmount required, but a future renaming or a
  // partial response shouldn't crash the parser — surface hashrate
  // alone with daily_sats unset.
  constexpr const char* body = R"([
    {"startTime":"2024-01-02T00:00:00.000+00:00",
     "endTime":"2024-01-03T00:00:00.000+00:00",
     "hashrate":50000000000}
  ])";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "50000000000");
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("foundry::parse — totalAmount=0 still records 0 sats") {
  // A fresh subaccount that mined nothing yet today should report
  // daily_sats=0 with has_daily_sats=true so the renderer can show
  // "0 SATS" instead of the "no data yet" placeholder.
  constexpr const char* body = R"([
    {"startTime":"2024-01-02T00:00:00.000+00:00",
     "endTime":"2024-01-03T00:00:00.000+00:00",
     "totalAmount":0,
     "hashrate":0}
  ])";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 0);
}

TEST_CASE("foundry::parse — 1 BTC sanity round-trip") {
  // Make sure the BTC->sats conversion doesn't drift on whole numbers.
  constexpr const char* body = R"([
    {"startTime":"2024-01-02T00:00:00.000+00:00",
     "endTime":"2024-01-03T00:00:00.000+00:00",
     "totalAmount":1.0,
     "hashrate":1}
  ])";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 100000000);
}

TEST_CASE("foundry::parse — empty array => no sample, no error") {
  ParsedStats out;
  CHECK(parse("[]", out));
  CHECK(out.hashrate.empty());
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("foundry::parse — wrapped 'data' object accepted") {
  // Defensive against legacy wrappers some Foundry endpoints used.
  constexpr const char* body = R"({"data":[
    {"startTime":"2024-01-02T00:00:00.000+00:00",
     "endTime":"2024-01-03T00:00:00.000+00:00",
     "totalAmount":0.00000123,
     "hashrate":42}
  ]})";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "42");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 123);
}

TEST_CASE("foundry::parse — malformed JSON => fail") {
  ParsedStats out;
  CHECK_FALSE(parse("not json", out));
}

TEST_CASE("foundry::parse — string hashrate fallback (legacy)") {
  // Foundry's older shape returned hashrate as a string. Keep the
  // fallback so a mid-rollout response that quotes the number doesn't
  // zero out the screen.
  constexpr const char* body = R"([
    {"hashrate":"98765432","totalAmount":0.0}
  ])";
  ParsedStats out;
  CHECK(parse(body, out));
  CHECK(out.hashrate == "98765432");
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 0);
}

}  // namespace
