// Host tests for pure-logic helpers in btclock_data_parse.hpp.
//
// Proves the blockfee2 2-decimal frame round-trips exactly through our
// extraction helpers, which is the critical invariant for the new
// DataSource path.

#include <cstdint>
#include <string>

#include "btclock_data_parse.hpp"
#include "doctest.h"

using btclock::parse::ExtractJsonInt;
using btclock::parse::ExtractJsonNumber;

TEST_CASE("blockfee2: synthetic 2-decimal frame parses to exact double") {
  const std::string frame = R"({"blockfee2": 12.75})";
  double v = 0;
  CHECK(ExtractJsonNumber(frame, "blockfee2", v));
  CHECK(v == doctest::Approx(12.75).epsilon(1e-9));
}

TEST_CASE("blockfee (integer) still parses as a number") {
  const std::string frame = R"({"blockfee": 13})";
  double v = 0;
  CHECK(ExtractJsonNumber(frame, "blockfee", v));
  CHECK(v == doctest::Approx(13.0).epsilon(1e-9));

  int64_t i = 0;
  CHECK(ExtractJsonInt(frame, "blockfee", i));
  CHECK(i == 13);
}

TEST_CASE("blockfee2 with whitespace and trailing fields") {
  const std::string frame =
      R"({ "blockheight" : 870123 ,  "blockfee2":12.75 })";
  double v = 0;
  CHECK(ExtractJsonNumber(frame, "blockfee2", v));
  CHECK(v == doctest::Approx(12.75).epsilon(1e-9));

  int64_t h = 0;
  CHECK(ExtractJsonInt(frame, "blockheight", h));
  CHECK(h == 870123);
}

TEST_CASE("blockfee2: negative / exponent forms are accepted") {
  double v = 0;
  CHECK(ExtractJsonNumber(R"({"blockfee2": -0.5})", "blockfee2", v));
  CHECK(v == doctest::Approx(-0.5).epsilon(1e-9));
  CHECK(ExtractJsonNumber(R"({"blockfee2": 1.275e1})", "blockfee2", v));
  CHECK(v == doctest::Approx(12.75).epsilon(1e-9));
}

TEST_CASE("missing field returns false and does not modify out") {
  const std::string frame = R"({"blockheight": 870123})";
  double v = -1;
  CHECK_FALSE(ExtractJsonNumber(frame, "blockfee2", v));
  CHECK(v == -1);
}

TEST_CASE("key appearing inside a value is not matched") {
  // "blockfee2" appears as a price-map key prefix; must not match the
  // top-level filter (we only accept keys preceded by { or ,).
  const std::string frame =
      R"({"price": {"blockfee2masquerade": "x"}, "blockheight": 9})";
  double v = 0;
  CHECK_FALSE(ExtractJsonNumber(frame, "blockfee2", v));
}

TEST_CASE("string-encoded number is rejected") {
  // Our helper extracts JSON numbers only. If the publisher starts
  // emitting quoted strings, callers must use a different path.
  const std::string frame = R"({"blockfee2": "12.75"})";
  double v = 0;
  CHECK_FALSE(ExtractJsonNumber(frame, "blockfee2", v));
}
