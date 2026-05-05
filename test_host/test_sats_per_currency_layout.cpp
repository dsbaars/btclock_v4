// Pin ComputeSatsPerCurrencyLayout against the regression that motivated
// it: weak-fiat currencies (VND, IRR, LBP, …) where 1 currency unit is
// worth less than 1 sat. The pre-existing helper rounded to int32 and
// surfaced "0" or all-blank for these — losing the precision that
// matters most for these codes. The new helper falls into a "0.dddd"
// path filling every digit cell and honours the global decimalShareDot
// pref to fold "0" + "." into a single merged cell when set.

#include <array>
#include <string>

#include "doctest.h"
#include "screens/sats_per_currency_layout.hpp"

namespace {

// Helper: stringify the cell array verbatim (each cell is its own
// substring). Single-char cells stay single-char; "0." stays merged;
// blanks render as a literal "_" so the assertions read top-to-bottom
// with no surprise empty tokens. Use TWO underscores per blank only if
// you want collisions with literal '_' chars (none here).
template <std::size_t Slots>
std::string CellsAsString(const btclock::SatsPerCurrencyLayout<Slots>& l) {
  std::string s;
  s.reserve(Slots * 2);
  for (std::size_t i = 0; i < Slots; ++i) {
    if (i) s.push_back('|');
    s.append(l.cells[i].empty() ? std::string("_") : l.cells[i]);
  }
  return s;
}

}  // namespace

TEST_CASE("ComputeSatsPerCurrencyLayout: integer path — USD ~$60k") {
  // 1e8 / 60000 ≈ 1666.67 → 1667 sats. 6 slots, glyph at index 1:
  // [_, STS, "1", "6", "6", "7"].
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "60000.0", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "_|_|1|6|6|7");
  CHECK(l.is_sats[1]);
  for (std::size_t i = 0; i < 6; ++i) {
    if (i != 1) CHECK_FALSE(l.is_sats[i]);
  }
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer path — use_symbol=false drops "
    "glyph") {
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "60000.0", /*use_symbol=*/false, /*share_dot=*/false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "_|_|1|6|6|7");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: integer path — share_dot is a no-op") {
  // share_dot only affects the fractional path (no '.' to fold on the
  // integer path). Pin both calls return the same cells.
  auto l_no = btclock::ComputeSatsPerCurrencyLayout<6>(
      "60000.0", /*use_symbol=*/true, /*share_dot=*/false);
  auto l_yes = btclock::ComputeSatsPerCurrencyLayout<6>(
      "60000.0", /*use_symbol=*/true, /*share_dot=*/true);
  CHECK(CellsAsString(l_no) == CellsAsString(l_yes));
  CHECK(l_no.is_sats == l_yes.is_sats);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional path — VND-scale price") {
  // 1 BTC at ~2.55B VND → 1e8 / 2.55e9 ≈ 0.0392 sats per VND. 6 slots,
  // share_dot=false: ["0", ".", "0", "3", "9", "2"].
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "2550000000.0", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK(l.fractional);
  CHECK(CellsAsString(l) == "0|.|0|3|9|2");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: fractional path — share_dot folds 0+. into "
    "one cell") {
  // share_dot=true: same input gets one extra fractional digit because
  // "0." merges into a single cell. 6 slots: ["0.", "0", "3", "9", "2", "2"].
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "2550000000.0", /*use_symbol=*/true, /*share_dot=*/true);
  CHECK(l.fractional);
  CHECK(CellsAsString(l) == "0.|0|3|9|2|2");
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional path — V8 (7 slots)") {
  // 7 slots, share_dot=false: ["0", ".", "0", "3", "9", "2", "2"].
  auto l = btclock::ComputeSatsPerCurrencyLayout<7>(
      "2550000000.0", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK(l.fractional);
  CHECK(CellsAsString(l) == "0|.|0|3|9|2|2");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: fractional path — V8 share_dot adds a "
    "digit") {
  // 7 slots, share_dot=true: one extra fractional digit fits.
  auto l = btclock::ComputeSatsPerCurrencyLayout<7>(
      "2550000000.0", /*use_symbol=*/true, /*share_dot=*/true);
  CHECK(l.fractional);
  CHECK(CellsAsString(l) == "0.|0|3|9|2|1|6");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: fractional path — values just under 1") {
  // sats_d ≈ 0.5 — used to round to int(1) which lied to the user. Now
  // shows the actual ratio.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "200000000.0", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK(l.fractional);
  CHECK(CellsAsString(l) == "0|.|5|0|0|0");
}

TEST_CASE("ComputeSatsPerCurrencyLayout: fractional rounds to 1 falls back") {
  // sats_d ≈ 0.99996 — %.4f formats as "1.0000". Falling back to the
  // integer path and rendering '1' in the trailing slot keeps the
  // integer-looks-like-integer invariant.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "100004000.0", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "_|_|_|_|_|1");
  CHECK(l.is_sats[4]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: blank on parse failure") {
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>(
      "garbage", /*use_symbol=*/true, /*share_dot=*/false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "_|_|_|_|_|_");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}

TEST_CASE("ComputeSatsPerCurrencyLayout: blank on zero / negative price") {
  CHECK(CellsAsString(btclock::ComputeSatsPerCurrencyLayout<6>(
            "0", true, false)) == "_|_|_|_|_|_");
  CHECK(CellsAsString(btclock::ComputeSatsPerCurrencyLayout<6>(
            "-1", true, false)) == "_|_|_|_|_|_");
  CHECK(CellsAsString(btclock::ComputeSatsPerCurrencyLayout<6>(
            "", true, false)) == "_|_|_|_|_|_");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer overflow into 6 slots truncates") {
  // 1 BTC at $0.01 → 1e10 sats per USD. Way above the int32 clamp
  // (4e9) — layout returns blank rather than wrapping.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("0.01", true, false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "_|_|_|_|_|_");
}

TEST_CASE(
    "ComputeSatsPerCurrencyLayout: integer overflows digit slots truncates") {
  // 1e8 / 0.5 = 200000000 → 9 digits; 6-slot board keeps the trailing 6.
  auto l = btclock::ComputeSatsPerCurrencyLayout<6>("0.50", true, false);
  CHECK_FALSE(l.fractional);
  CHECK(CellsAsString(l) == "0|0|0|0|0|0");
  for (std::size_t i = 0; i < 6; ++i) CHECK_FALSE(l.is_sats[i]);
}
