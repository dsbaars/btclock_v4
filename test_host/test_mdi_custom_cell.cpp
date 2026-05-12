#include "doctest.h"
#include "mdi_codepoints.hpp"
#include "mdi_custom_cell.hpp"

using btclock::ParseCustomCellMdi;
using btclock::mdi::kIconBitcoin;
using btclock::mdi::kIconCurrencyBtc;
using btclock::mdi::kIconWifi;

TEST_CASE("ParseCustomCellMdi: plain text is not mdi") {
  std::uint32_t cp = 999;
  CHECK_FALSE(ParseCustomCellMdi("hello", &cp));
  CHECK(cp == 999);
}

TEST_CASE("ParseCustomCellMdi: mdi wifi resolves") {
  std::uint32_t cp = 0;
  REQUIRE(ParseCustomCellMdi("mdi:wifi", &cp));
  CHECK(cp == kIconWifi);
}

TEST_CASE("ParseCustomCellMdi: case-insensitive prefix and name") {
  std::uint32_t cp = 0;
  REQUIRE(ParseCustomCellMdi("MDI:WiFi", &cp));
  CHECK(cp == kIconWifi);
}

TEST_CASE("ParseCustomCellMdi: surrounding whitespace") {
  std::uint32_t cp = 0;
  REQUIRE(ParseCustomCellMdi("  mdi:bitcoin  ", &cp));
  CHECK(cp == kIconBitcoin);
}

TEST_CASE("ParseCustomCellMdi: currency-btc resolves") {
  // Pins the regen_mdi.sh subset extension — the icon is in the
  // bundle but not yet painted by any screen. Available through
  // POST /api/show/custom for ad-hoc panels.
  std::uint32_t cp = 0;
  REQUIRE(ParseCustomCellMdi("mdi:currency-btc", &cp));
  CHECK(cp == kIconCurrencyBtc);
}

TEST_CASE("ParseCustomCellMdi: bare mdi: is blank") {
  std::uint32_t cp = 999;
  REQUIRE(ParseCustomCellMdi("mdi:", &cp));
  CHECK(cp == 0);
}

TEST_CASE("ParseCustomCellMdi: unknown icon name yields blank") {
  std::uint32_t cp = 999;
  REQUIRE(ParseCustomCellMdi("mdi:not-in-subset", &cp));
  CHECK(cp == 0);
}

TEST_CASE("ParseCustomCellMdi: slash in icon token falls through") {
  std::uint32_t cp = 999;
  CHECK_FALSE(ParseCustomCellMdi("mdi:wifi/extra", &cp));
  CHECK(cp == 999);
}

TEST_CASE("ParseCustomCellMdi: invalid characters in name fall through") {
  std::uint32_t cp = 999;
  CHECK_FALSE(ParseCustomCellMdi("mdi:wifi extra", &cp));
  CHECK(cp == 999);
}
