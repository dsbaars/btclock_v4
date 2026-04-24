// Host tests for the BTC-price screen layout helper.
//
// Exercises the pure-logic helper in main/screens/price_layout.hpp —
// the header is header-only and doesn't include any ESP-IDF APIs, so
// we can include it directly without the framebuffer / EPD dependency.
//
// Coverage focus (btclock_v3_fci-lx0.12):
//   - Sub-$100k prices get 1 decimal place where the digit + '.' fits.
//   - Sub-$100  prices get 2 decimal places.
//   - Sub-$1    prices get 3 decimal places (the whole reason this
//     feature exists — altcoin-scale tickers the old Arduino firmware
//     would round to "$0").
//   - Symbol panel is dropped before decimals are dropped when the
//     chosen precision doesn't fit alongside the currency glyph.
//   - Integer >= $100k keeps the old integer-only behaviour byte-for-
//     byte (the 8/7-char overflow drop-glyph path).
//   - Dedicated '.' cell: the dot lives in one slot on its own, not
//     folded into an adjacent digit (mirrors parsePriceData shareDot=
//     false in the old firmware's suffix mode).

#include "doctest.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#include "screens/price_layout.hpp"

namespace {

constexpr std::size_t kSlots7 = 6;  // 7-panel board → 6 digit cells
constexpr std::size_t kSlots8 = 7;  // 8-panel board → 7 digit cells

template <std::size_t Slots>
std::string Render(const std::array<char, Slots>& digits,
                   const std::array<bool, Slots>& is_sym,
                   char sym_char = '$') {
  std::string s(Slots, ' ');
  for (std::size_t i = 0; i < Slots; ++i) {
    if (is_sym[i]) s[i] = sym_char;
    else s[i] = digits[i];
  }
  return s;
}

std::string ReadFile(const std::string& relpath) {
  const std::string path = std::string(BTCLOCK_POC_ROOT) + "/" + relpath;
  std::ifstream f(path);
  REQUIRE_MESSAGE(f.is_open(), "could not open " << path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("price_layout — integer >= 100k keeps old integer path") {
  // Old parsePriceData behaviour: a 6-digit integer on a 6-slot (7-
  // panel) digit region drops the `$` glyph — the number is given the
  // full width. `$102345` is 7 chars so it overflows; the layout keeps
  // the 6-digit number and drops the symbol, matching the old firmware.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(102345.0, /*use_symbol=*/true, digits,
                                   is_sym);
  CHECK(Render(digits, is_sym) == "102345");

  // Sub-100k-chars regime: 5-digit integer keeps the `$` — 6 chars fits
  // exactly. No decimals are added because $100k is the cutoff above
  // which we preserve the old integer-only behaviour unchanged.
  btclock::LayoutBtcPrice<kSlots7>(78583.0, true, digits, is_sym);
  // 78583 >= 100 so 1 decimal is requested, but "$78583.0" = 8 chars
  // won't fit; "78583.0" = 7 chars won't fit either; fall through to
  // integer "$78583" = 6 chars ✓.
  CHECK(Render(digits, is_sym) == "$78583");
}

TEST_CASE("price_layout — 6-digit integer overflow drops symbol") {
  // Matches old firmware parsePriceData: a 6-digit integer on a 6-slot
  // (7-panel) board can't keep the `$` symbol. No '.' anywhere.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(999999.0, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "999999");
}

TEST_CASE("price_layout — sub-100k (1000-9999) gets 1 decimal, drops symbol") {
  // $7858.3 needs 6 chars for "7858.3" (no symbol) or 7 chars with "$".
  // 6-slot board can't fit the symbol without dropping the decimal —
  // and precision > glyph, so we drop the glyph.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(7858.3, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "7858.3");
  // No symbol cell flagged.
  for (bool s : is_sym) CHECK(!s);
}

TEST_CASE("price_layout — sub-1000 (100-999) gets 1 decimal with symbol") {
  // $123.4 = "$123.4" = 6 chars fits exactly on 7-panel.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(123.4, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "$123.4");
  // Dot is in its own cell; symbol cell is flagged (slot 0).
  CHECK(is_sym[0]);
  CHECK(digits[4] == '.');
}

TEST_CASE("price_layout — sub-100 (1-99) gets 2 decimals with symbol") {
  // $45.67 = "$45.67" = 6 chars fits on 7-panel.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(45.67, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "$45.67");
  CHECK(is_sym[0]);
  CHECK(digits[3] == '.');
}

TEST_CASE("price_layout — sub-dollar gets 3 decimals with symbol") {
  // The lx0.12 primary use case: altcoin-scale "$0.123" tickers should
  // show three decimals rather than rounding to $0. "$0.123" = 6 chars.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(0.123, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "$0.123");
  CHECK(is_sym[0]);
  CHECK(digits[2] == '.');
}

TEST_CASE("price_layout — V8 8-panel is integer-only, no trailing dot+0") {
  // V8 mirrors the old firmware's 8-panel `parsePriceData` behaviour
  // (useSuffixFormat=false, shareDot=false): 7 integer digits + glyph,
  // no '.' cell. Bug 2 — the previous layout reused Rev A/B sub-dollar
  // decimal precision, which on V8 produced a ". 0" tail for integer-
  // rounded prices (e.g. 7858.3 → "$7858.3" spanning two extra cells
  // that the on-panel renderer filled with ". 0" artefacts).
  std::array<char, kSlots8> digits;
  std::array<bool, kSlots8> is_sym;
  btclock::LayoutBtcPrice<kSlots8>(7858.3, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "  $7858");
  CHECK(is_sym[2]);
  // No '.' cell anywhere in the layout.
  for (char c : digits) CHECK(c != '.');
}

TEST_CASE("price_layout — V8 8-panel preserves 6-digit integer + glyph") {
  // 999999 is 6 chars; with the glyph that's 7 → fits exactly in 7 slots.
  std::array<char, kSlots8> digits;
  std::array<bool, kSlots8> is_sym;
  btclock::LayoutBtcPrice<kSlots8>(999999.0, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "$999999");
  CHECK(is_sym[0]);
}

TEST_CASE("price_layout — V8 8-panel sub-dollar rounds rather than emits decimals") {
  // Altcoin-scale prices round to 0 on V8 (integer-only path). The old
  // firmware parity's integer branch did the same: sub-$1 tickers got
  // their value rounded away. V8 boards' 8-panel layout matches that.
  std::array<char, kSlots8> digits;
  std::array<bool, kSlots8> is_sym;
  btclock::LayoutBtcPrice<kSlots8>(0.45, true, digits, is_sym);
  CHECK(Render(digits, is_sym) == "     $0");
  for (char c : digits) CHECK(c != '.');
}

TEST_CASE("price_layout — invalid / negative prices blank the cells") {
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  CHECK(!btclock::LayoutBtcPrice<kSlots7>(-1.0, true, digits, is_sym));
  for (char c : digits) CHECK(c == ' ');
  for (bool s : is_sym) CHECK(!s);
}

TEST_CASE("price_layout — PriceDecimalPlaces thresholds") {
  CHECK(btclock::PriceDecimalPlaces(200000.0) == 0);
  CHECK(btclock::PriceDecimalPlaces(100000.0) == 0);
  CHECK(btclock::PriceDecimalPlaces(99999.99) == 1);
  CHECK(btclock::PriceDecimalPlaces(100.0)    == 1);
  CHECK(btclock::PriceDecimalPlaces(99.99)    == 2);
  CHECK(btclock::PriceDecimalPlaces(1.0)      == 2);
  CHECK(btclock::PriceDecimalPlaces(0.99)     == 3);
  CHECK(btclock::PriceDecimalPlaces(0.01)     == 3);
  // Sub-cent → integer fallback rather than "0.000" noise.
  CHECK(btclock::PriceDecimalPlaces(0.001)    == 0);
  CHECK(btclock::PriceDecimalPlaces(0.0)      == 0);
  CHECK(btclock::PriceDecimalPlaces(-1.0)     == 0);
}

TEST_CASE("price_layout — no-symbol currency keeps decimal layout") {
  // A currency without a mapped UTF-8 glyph (e.g. AUD, CAD) passes
  // use_symbol=false; the layout should still emit the decimal form
  // right-justified without a symbol cell.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(45.67, /*use_symbol=*/false, digits,
                                   is_sym);
  CHECK(Render(digits, is_sym) == " 45.67");
  for (bool s : is_sym) CHECK(!s);
}

TEST_CASE("price_layout — LayoutBtcPriceStrings mirrors renderer cells") {
  // The WebUI mirror path goes through LayoutBtcPriceStrings — the
  // returned array is one std::string per cell, with the UTF-8 glyph
  // filled in where the boolean layout had is_sym=true.
  const auto out =
      btclock::LayoutBtcPriceStrings<kSlots7>(45.67, "$");
  CHECK(out[0] == "$");
  CHECK(out[1] == "4");
  CHECK(out[2] == "5");
  CHECK(out[3] == ".");
  CHECK(out[4] == "6");
  CHECK(out[5] == "7");
}

TEST_CASE("price_layout — LayoutBtcPriceStrings blanks on invalid price") {
  const auto out =
      btclock::LayoutBtcPriceStrings<kSlots7>(-1.0, "$");
  for (const auto& s : out) CHECK(s.empty());
}

TEST_CASE("price_layout — dot is emitted in a dedicated cell") {
  // Double-check: the '.' never shares a slot with an adjacent digit
  // (no "5." or ".6" multi-char cells). Mirrors parsePriceData's
  // default shareDot=false suffix layout in the old firmware — and the
  // renderer uses a dot-inclusive ref scoped to this screen to keep the
  // dot glyph's baseline aligned with the digits next door.
  std::array<char, kSlots7> digits;
  std::array<bool, kSlots7> is_sym;
  btclock::LayoutBtcPrice<kSlots7>(12.34, true, digits, is_sym);
  int dot_cells = 0;
  for (char c : digits) if (c == '.') ++dot_cells;
  CHECK(dot_cells == 1);
}

TEST_CASE("price_layout — header defines kPriceDotRef local to this screen") {
  // Regression guard paired with test_screen_ref_chars.cpp: the dot-
  // inclusive ref used by btc_price.cpp must live in the price_layout
  // header (scoped to the screen), not in common.hpp.
  const std::string hdr = ReadFile("main/screens/price_layout.hpp");
  CHECK(hdr.find("kPriceDotRef") != std::string::npos);
  CHECK(hdr.find("\"0123456789.\"") != std::string::npos);

  const std::string common = ReadFile("main/screens/common.hpp");
  CHECK(common.find("kPriceDotRef") == std::string::npos);
  CHECK(common.find("\"0123456789.\"") == std::string::npos);
}
