// Pin ParseCurrenciesJson against the live `/api/v2/currencies` shape
// (a JSON array of 3-letter ISO codes) and the defensive cases that
// matter at boot: malformed JSON, wrong top-level type, embedded
// non-string entries, codes the firmware shouldn't accept (lowercase,
// digits, wrong length), and duplicate dedup.

#include <string>
#include <vector>

#include "btclock_currencies_parse.hpp"
#include "doctest.h"

namespace {

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v) {
    if (x == s) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("ParseCurrenciesJson: canonical upstream payload") {
  const std::string body = R"(["USD","AUD","GBP","JPY","EUR","CAD","CHF"])";
  auto out = btclock::ParseCurrenciesJson(body);
  REQUIRE(out.size() == 7);
  CHECK(Contains(out, "USD"));
  CHECK(Contains(out, "AUD"));
  CHECK(Contains(out, "GBP"));
  CHECK(Contains(out, "JPY"));
  CHECK(Contains(out, "EUR"));
  CHECK(Contains(out, "CAD"));
  CHECK(Contains(out, "CHF"));
}

TEST_CASE("ParseCurrenciesJson: preserves array order") {
  const std::string body = R"(["JPY","USD","EUR"])";
  auto out = btclock::ParseCurrenciesJson(body);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == "JPY");
  CHECK(out[1] == "USD");
  CHECK(out[2] == "EUR");
}

TEST_CASE("ParseCurrenciesJson: drops malformed entries silently") {
  // Defensive cases: lowercase, wrong-length, digit-bearing, non-string.
  // Each is dropped; the rest of the array still parses.
  const std::string body =
      R"(["USD","usd","US","USDD","US1","",null,42,{"x":1},"EUR"])";
  auto out = btclock::ParseCurrenciesJson(body);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "USD");
  CHECK(out[1] == "EUR");
}

TEST_CASE("ParseCurrenciesJson: deduplicates while preserving first-seen") {
  const std::string body = R"(["USD","EUR","USD","EUR","JPY","USD"])";
  auto out = btclock::ParseCurrenciesJson(body);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == "USD");
  CHECK(out[1] == "EUR");
  CHECK(out[2] == "JPY");
}

TEST_CASE("ParseCurrenciesJson: rejects non-array top-level") {
  CHECK(btclock::ParseCurrenciesJson(R"({"USD":1})").empty());
  CHECK(btclock::ParseCurrenciesJson(R"("USD")").empty());
  CHECK(btclock::ParseCurrenciesJson(R"(42)").empty());
  CHECK(btclock::ParseCurrenciesJson(R"(null)").empty());
}

TEST_CASE("ParseCurrenciesJson: empty / malformed bodies") {
  CHECK(btclock::ParseCurrenciesJson("").empty());
  CHECK(btclock::ParseCurrenciesJson(nullptr, 0).empty());
  CHECK(btclock::ParseCurrenciesJson("[").empty());
  CHECK(btclock::ParseCurrenciesJson("not json").empty());
  // HTML — what a captive portal would serve.
  CHECK(btclock::ParseCurrenciesJson("<!DOCTYPE html>").empty());
}

TEST_CASE("ParseCurrenciesJson: empty array returns empty") {
  CHECK(btclock::ParseCurrenciesJson(R"([])").empty());
}

TEST_CASE("ParseCurrenciesJson: tolerates whitespace") {
  const std::string body = "[ \"USD\" ,\n  \"EUR\"\t]";
  auto out = btclock::ParseCurrenciesJson(body);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "USD");
  CHECK(out[1] == "EUR");
}
