// Host-only parser test for the Bitaxe AxeOS /api/system/info shape.

#include <string>

#include "bitaxe/bitaxe_parser.hpp"
#include "doctest.h"

namespace {
using btclock::bitaxe::FormatBestDiff;
using btclock::bitaxe::Parse;
using btclock::bitaxe::ParsedStats;
}  // namespace

TEST_CASE("bitaxe::Parse — full payload with string bestDiff") {
  constexpr const char* body = R"({
    "hashRate": 1235.4,
    "bestDiff": "15.6M",
    "sharesAccepted": 4200,
    "temp": 62.5,
    "power": 14.2
  })";
  ParsedStats p;
  CHECK(Parse(body, p));
  REQUIRE(p.hashrate_ghs.has_value());
  CHECK(*p.hashrate_ghs == doctest::Approx(1235.4));
  REQUIRE(p.best_diff.has_value());
  CHECK(*p.best_diff == "15.6M");
  REQUIRE(p.shares_accepted.has_value());
  CHECK(*p.shares_accepted == 4200);
  REQUIRE(p.temperature_c.has_value());
  CHECK(*p.temperature_c == doctest::Approx(62.5));
}

TEST_CASE("bitaxe::Parse — numeric bestDiff gets canonicalised") {
  // Newer AxeOS firmware emits bestDiff as a raw number. Parser must
  // render it with the same K/M/G ladder as the old firmware's
  // parseBitaxeBestDiff so the EPD layout stays stable.
  constexpr const char* body = R"({
    "hashRate": 450,
    "bestDiff": 1560000
  })";
  ParsedStats p;
  CHECK(Parse(body, p));
  REQUIRE(p.best_diff.has_value());
  CHECK(*p.best_diff == "1.6M");
}

TEST_CASE("bitaxe::Parse — missing fields leave optionals empty") {
  // Only hashRate present — older AxeOS fork response. Parser should
  // succeed and only populate what's there; the hub merge then leaves
  // the rest of DataSnapshot.bitaxe untouched.
  constexpr const char* body = R"({"hashRate": 999.0})";
  ParsedStats p;
  CHECK(Parse(body, p));
  CHECK(p.hashrate_ghs.has_value());
  CHECK_FALSE(p.best_diff.has_value());
  CHECK_FALSE(p.shares_accepted.has_value());
  CHECK_FALSE(p.temperature_c.has_value());
}

TEST_CASE("bitaxe::Parse — accepts `temperature` alias for `temp`") {
  constexpr const char* body = R"({
    "hashRate": 1,
    "temperature": 55.0
  })";
  ParsedStats p;
  CHECK(Parse(body, p));
  REQUIRE(p.temperature_c.has_value());
  CHECK(*p.temperature_c == doctest::Approx(55.0));
}

TEST_CASE("bitaxe::Parse — malformed JSON fails") {
  ParsedStats p;
  CHECK_FALSE(Parse("not json", p));
}

TEST_CASE("bitaxe::Parse — non-object root fails") {
  ParsedStats p;
  CHECK_FALSE(Parse("[1,2,3]", p));
}

TEST_CASE("FormatBestDiff — K/M/G ladder") {
  CHECK(FormatBestDiff(0.0) == "0");
  CHECK(FormatBestDiff(999.0) == "999");
  CHECK(FormatBestDiff(1500.0) == "1.5K");
  CHECK(FormatBestDiff(15'600'000.0) == "15.6M");
  CHECK(FormatBestDiff(1'500'000'000.0) == "1.5G");
  CHECK(FormatBestDiff(12'000'000'000.0) == "12G");
  CHECK(FormatBestDiff(1.5e12) == "1.5T");
  CHECK(FormatBestDiff(1.5e15) == "1.5P");
}

TEST_CASE("FormatBestDiff — trailing .0 trimmed") {
  // "1.0M" is visually noisier than "1M" on the EPD and wastes a cell.
  CHECK(FormatBestDiff(1'000'000.0) == "1M");
}
