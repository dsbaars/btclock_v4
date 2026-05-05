// Pin ComputeSatsPerCurrencyLayout against the regression that motivated
// it: weak-fiat currencies (VND, IRR, LBP, …) where 1 currency unit is
// worth less than 1 sat. The pre-existing helper rounded to int32 and
// surfaced "0" or all-blank for these — losing the precision that
// matters most for these codes. The new helper falls into a "0.dddd"
// path filling every digit cell.

#include <array>
#include <string>

#include "doctest.h"
#include "screens/sats_per_currency_layout.hpp"

namespace {

// Helper: pull the layout into a std::string (chars only — '.' stays
// literal) so the assertions read top-to-bottom with the actual digits.
template <std::size_t Slots>
std::string DigitsAsString(const btclock::SatsPerCurrencyLayout<Slots>& l) {
  std::string s;
  s.reserve(Slots);
  for (std::size_t i = 0; i < Slots; ++i) s.push_back(l.digits[i]);
  return s;
}

}  // namespace

TEST_CASE("ComputeSatsPerCurrencyLayout: integer path — USD ~$60k") {
  // 1e8 / 60000 ≈ 1666.67 → 1667 sats. 6 slots: "  1667" (sats glyph
  // anchored before the leading digit when use_symbol is true).
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("60000.0",
                                                    /*use_symbol=*/true);
  CHECK_FALSE(l.fractional);
  CHECK(DigitsAsString(l) == "  1667");
  // Sats glyph sits one slot before the first digit, so cell index 1.
  CHECK(l.is_sats[1]);
  // No other glyph cells.
  for (std::size_t i = 0; i < 6; ++i) {
    if (i != 1) CHECK_FALSE(l.is_sats[i]);
  }
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer path — use_symbol=false drops "
    "glyph") {
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("60000.0",
                                                    /*use_symbol=*/false);
  CHECK_FALSE(l.fractional);
  CHECK(DigitsAsString(l) == "  1667");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional path — VND-scale price") {
  // 1 BTC at ~2.55B VND → 1e8 / 2.55e9 ≈ 0.0392 sats per VND. With
  // 6 slots, "0." reserves 2, leaves 4 fractional digits → "0.0392".
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("2550000000.0",
                                                    /*use_symbol=*/true);
  CHECK(l.fractional);
  CHECK(DigitsAsString(l) == "0.0392");
  // Fractional path leaves is_sats all-false — there's no separate
  // glyph cell, the layout already fills every slot with the number.
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional path — V8 (7 slots)") {
  // 7 slots (V8 8-panel): "0." + 5 fractional digits → "0.03922".
  auto l = btclock::ComputeSatsPerCurrencyLayout<7>("2550000000.0",
                                                    /*use_symbol=*/true);
  CHECK(l.fractional);
  CHECK(DigitsAsString(l) == "0.03922");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: fractional path — values just under 1") {
  // sats_d ≈ 0.5 — used to round to int(1) which lied to the user. Now
  // shows the actual ratio.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("200000000.0",
                                                    /*use_symbol=*/true);
  CHECK(l.fractional);
  CHECK(DigitsAsString(l) == "0.5000");
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional rounds to 1 falls back") {
  // sats_d ≈ 0.99996 — %.4f formats as "1.0000". Falling back to the
  // integer path and rendering '1' in the trailing slot keeps the
  // integer-looks-like-integer invariant. Sats glyph (use_symbol=true)
  // anchors one slot before.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("100004000.0",
                                                    /*use_symbol=*/true);
  CHECK_FALSE(l.fractional);
  CHECK(DigitsAsString(l) == "     1");
  CHECK(l.is_sats[4]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: blank on parse failure") {
  auto l =
      btclock::ComputeSatsPerCurrencyLayout<6>("garbage", /*use_symbol=*/true);
  CHECK_FALSE(l.fractional);
  CHECK(DigitsAsString(l) == "      ");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: blank on zero / negative price") {
  CHECK(DigitsAsString(btclock::ComputeSatsPerCurrencyLayout<6>("0", true)) ==
        "      ");
  CHECK(DigitsAsString(btclock::ComputeSatsPerCurrencyLayout<6>("-1", true)) ==
        "      ");
  CHECK(DigitsAsString(btclock::ComputeSatsPerCurrencyLayout<6>("", true)) ==
        "      ");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer overflow into 6 slots truncates") {
  // 1 BTC at $0.01 → 1e10 sats per USD. Way above the int32 clamp
  // (4e9) — layout returns blank rather than wrapping. (A price this
  // small is a data-feed bug, not a real currency.)
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("0.01", true);
  CHECK_FALSE(l.fractional);
  CHECK(DigitsAsString(l) == "      ");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer overflows digit slots truncates") {
  // 9-digit sats overflow a 6-slot board. The integer path keeps the
  // low-order digits — same behaviour as the old ComputeMoscowLayoutN.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("0.50", true);
  CHECK_FALSE(l.fractional);
  // 1e8 / 0.5 = 200000000 → 9 digits, keep last 6.
  CHECK(DigitsAsString(l) == "000000");
  // Glyph slot doesn't apply on overflow (no leading pad).
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}
