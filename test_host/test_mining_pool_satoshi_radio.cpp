// Host-only test confirming Satoshi Radio's parser is the shared
// ckpool-family walker. The pool's API is identical in shape to
// Noderunners/ckpool — if the shared walker is ever modified in a
// way that breaks it, this test pins the expected behaviour.
//
// (Full-class test lives on target; the header drags in FreeRTOS +
// esp_http_client.)

#include <string>

#include "doctest.h"
#include "mining_pool_common/ckpool_family_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::ckpool_family::parse;

TEST_CASE("satoshi_radio: typical T-unit response") {
  ParsedStats out;

  // Sample shape captured from
  //   curl https://pool.satoshiradio.nl/api/v1/pool
  // on 2026-03-14. Full response had additional fields
  // (workerCount, runtime, etc.) that the parser ignores.
  CHECK(parse(R"({"hashrate1m": "450T", "workerCount": 12})", out));
  CHECK(out.hashrate == "450000000000000");
  CHECK_FALSE(out.has_daily_sats);
  CHECK_FALSE(out.has_workers);
}

TEST_CASE("satoshi_radio: E-unit response (large pool)") {
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "2.5E"})", out));
  CHECK(out.hashrate == "2500000000000000000");
}

TEST_CASE("satoshi_radio: offline user => \"0\" => empty") {
  ParsedStats out;
  CHECK(parse(R"({"hashrate1m": "0"})", out));
  CHECK(out.hashrate.empty());
}

}  // namespace
