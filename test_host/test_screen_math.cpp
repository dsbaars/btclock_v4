#include <cstdint>
#include <cstring>
#include <limits>

#include "doctest.h"
#include "screens/screen_math.hpp"

// ----- HalvingCountdown -----

TEST_CASE("HalvingCountdown at block 0 = full interval") {
  CHECK(btclock::HalvingCountdown(0) == 210000);
}

TEST_CASE("HalvingCountdown at block 209999 = 1") {
  CHECK(btclock::HalvingCountdown(209999) == 1);
}

TEST_CASE("HalvingCountdown at the halving block resets to full interval") {
  // Old firmware does `210000 - (h % 210000)`; at h == 210000 that's
  // 210000 - 0 == 210000, not 0. We match that — rendering 0 on the
  // exact halving block for a single-block window would blink zeros
  // for 10 minutes and confuse people.
  CHECK(btclock::HalvingCountdown(210000) == 210000);
  CHECK(btclock::HalvingCountdown(420000) == 210000);
}

TEST_CASE("HalvingCountdown mid-era") {
  CHECK(btclock::HalvingCountdown(210000 + 12345) == 210000 - 12345);
}

// ----- SupplyAtBlock -----

TEST_CASE("SupplyAtBlock(0) == 0") {
  CHECK(btclock::SupplyAtBlock(0) == 0);
}

TEST_CASE("SupplyAtBlock at first halving == 10_500_000 BTC") {
  // 210000 blocks × 50 BTC = 10.5M BTC. Era 0 is fully completed at
  // the halving block itself in our model (h-=in_era then next era).
  CHECK(btclock::SupplyAtBlock(210000) == 10500000ULL);
}

TEST_CASE("SupplyAtBlock at second halving == 15_750_000 BTC") {
  // +210000 blocks × 25 BTC = +5.25M → 15.75M.
  CHECK(btclock::SupplyAtBlock(420000) == 15750000ULL);
}

TEST_CASE("SupplyAtBlock mid-era one after genesis") {
  // One block into era 0 → 50 BTC integer-truncated.
  CHECK(btclock::SupplyAtBlock(1) == 50ULL);
}

TEST_CASE("SupplyAtBlock plateaus once the reward rounds to zero") {
  // After 33 eras the per-block reward is 0 sats. Integer arithmetic
  // then stops adding — the plateau value is the floor of the old
  // firmware's 20999999.9769 double. We deliberately don't over-round
  // to a marketing "21,000,000": the display matches the real chain
  // supply, and the cap only exists to guard against arithmetic error
  // in an era we never reach.
  CHECK(btclock::SupplyAtBlock(33 * 210000 + 1) == 20999999ULL);
  CHECK(btclock::SupplyAtBlock(50 * 210000) == 20999999ULL);
  // The explicit cap still prevents runaway growth if reward math
  // ever overflows.
  CHECK(btclock::SupplyAtBlock(33 * 210000 + 1) <= btclock::kMaxSupplyBtc);
}

// ----- MarketCap -----

TEST_CASE("MarketCap is price_int * supply") {
  // 50_000 × 10_500_000 = 525_000_000_000
  CHECK(btclock::MarketCap(50000, 210000) == 525000000000ULL);
}

TEST_CASE("MarketCap round-trips through a known current-ish value") {
  // At ~block 840000 (four halvings in) supply is ≈ 19.687_500 M BTC
  // (50+25+12.5+6.25 = 93.75 BTC-per-block-average × 210000 × 4 eras
  // exactly: 10500000+5250000+2625000+1312500 = 19687500).
  CHECK(btclock::SupplyAtBlock(840000) == 19687500ULL);
  CHECK(btclock::MarketCap(100000, 840000) == 100000ULL * 19687500ULL);
}

// ----- FormatDigits64 -----

TEST_CASE("FormatDigits64 right-justifies with space padding") {
  char buf[8];
  btclock::FormatDigits64(12345, buf, 8);
  CHECK(std::memcmp(buf, "   12345", 8) == 0);
}

TEST_CASE("FormatDigits64 truncates leading digits on overflow") {
  char buf[4];
  btclock::FormatDigits64(1234567ULL, buf, 4);
  CHECK(std::memcmp(buf, "4567", 4) == 0);
}

TEST_CASE("FormatDigits64 of zero is a trailing zero with blank pad") {
  char buf[4];
  btclock::FormatDigits64(0, buf, 4);
  CHECK(std::memcmp(buf, "   0", 4) == 0);
}

// ----- ComputeClockLayout -----

TEST_CASE("ComputeClockLayout packs HH:MM into the trailing 5 slots") {
  const auto l = btclock::ComputeClockLayout(true, 14, 7, 6);
  // digit_panels=6 → base=1: slots[0]=' ', slots[1..5] = "14:07"
  CHECK(l.digits[0] == ' ');
  CHECK(l.digits[1] == '1');
  CHECK(l.digits[2] == '4');
  CHECK(l.digits[3] == ':');
  CHECK(l.digits[4] == '0');
  CHECK(l.digits[5] == '7');
}

TEST_CASE("ComputeClockLayout zero-pads single-digit hour and minute") {
  const auto l = btclock::ComputeClockLayout(true, 3, 9, 5);
  CHECK(l.digits[0] == '0');
  CHECK(l.digits[1] == '3');
  CHECK(l.digits[2] == ':');
  CHECK(l.digits[3] == '0');
  CHECK(l.digits[4] == '9');
}

TEST_CASE("ComputeClockLayout returns all-blanks when not valid") {
  const auto l = btclock::ComputeClockLayout(false, 14, 7, 6);
  for (int i = 0; i < 6; ++i) CHECK(l.digits[i] == ' ');
}

// ----- FormatNumberWithSuffix -----

TEST_CASE("FormatNumberWithSuffix ports old firmware output (no suffix)") {
  // Sub-thousand values: no suffix at all.
  CHECK(btclock::FormatNumberWithSuffix(0) == "0");
  CHECK(btclock::FormatNumberWithSuffix(42) == "42");
  CHECK(btclock::FormatNumberWithSuffix(999) == "999");
}

TEST_CASE("FormatNumberWithSuffix adds K/M/B/T at each decade") {
  // Decimal packing follows the available-character budget (default 4):
  // "1K" fits in 2 → pack one decimal digit to "1.0K".
  CHECK(btclock::FormatNumberWithSuffix(1000) == "1.0K");
  // budget=4: "1020825000000" → "1T" fits in 2, pack decimals to "1.0T".
  CHECK(btclock::FormatNumberWithSuffix(1020825000000ULL, 4) == "1.0T");
  // budget=5: same number → "1.02T" (one more decimal).
  CHECK(btclock::FormatNumberWithSuffix(1020825000000ULL, 5) == "1.02T");
  // Supply: 19.687M BTC at block 840000.
  CHECK(btclock::FormatNumberWithSuffix(19687500ULL, 5) == "19.7M");
}

TEST_CASE("FormatNumberWithSuffix MOW mode uses M across the thousands") {
  // MOW = million-of-wotsits — jumps straight to "M" below 1M.
  CHECK(btclock::FormatNumberWithSuffix(0, 4, true) == "0M");
  // 93600 in MOW with budget 5 → truncated ".093" * M → "0.093M".
  // The truncation shape follows old firmware's std::to_string + substr.
  const auto mow_93600 = btclock::FormatNumberWithSuffix(93600, 5, true);
  CHECK(mow_93600.back() == 'M');
}

// ----- BlockHeightDropsLabel -----

TEST_CASE("BlockHeightDropsLabel keeps label for 6-digit heights") {
  CHECK_FALSE(btclock::BlockHeightDropsLabel(999999u, 7));
  CHECK_FALSE(btclock::BlockHeightDropsLabel(1u, 7));
  CHECK_FALSE(btclock::BlockHeightDropsLabel(0u, 7));
}

TEST_CASE("BlockHeightDropsLabel drops label at 7-digit rollover") {
  // Mainnet passed block 900000 long ago; 1_000_000 is the point the
  // label must vanish or we silently lose the leading "1".
  CHECK(btclock::BlockHeightDropsLabel(1000000u, 7));
  CHECK(btclock::BlockHeightDropsLabel(1234567u, 7));
}

TEST_CASE("BlockHeightDropsLabel scales with panel count") {
  // 8-panel boards keep the label until 8 digits — future-proofed for
  // the v8 variant.
  CHECK_FALSE(btclock::BlockHeightDropsLabel(1000000u, 8));
  CHECK(btclock::BlockHeightDropsLabel(10000000u, 8));
}

TEST_CASE("ComputeClockLayout handles midnight and 23:59 edges") {
  const auto midnight = btclock::ComputeClockLayout(true, 0, 0, 5);
  CHECK(midnight.digits[0] == '0');
  CHECK(midnight.digits[1] == '0');
  CHECK(midnight.digits[2] == ':');
  CHECK(midnight.digits[3] == '0');
  CHECK(midnight.digits[4] == '0');
  const auto latest = btclock::ComputeClockLayout(true, 23, 59, 5);
  CHECK(latest.digits[0] == '2');
  CHECK(latest.digits[1] == '3');
  CHECK(latest.digits[2] == ':');
  CHECK(latest.digits[3] == '5');
  CHECK(latest.digits[4] == '9');
}

// ----- ComputeClockLayout: hide-leading-zero -----
//
// Mirrors the `hideLeadZero` setting. The tens-of-hours slot goes blank
// for 0..9; 10..23 stay untouched. Minute digits always keep their
// zero pad so "7:05" renders as " 7:05", never " 7: 5".

TEST_CASE("ComputeClockLayout hide_leading_zero blanks tens for 7am") {
  const auto l = btclock::ComputeClockLayout(true, 7, 0, 5, true);
  CHECK(l.digits[0] == ' ');
  CHECK(l.digits[1] == '7');
  CHECK(l.digits[2] == ':');
  CHECK(l.digits[3] == '0');
  CHECK(l.digits[4] == '0');
}

TEST_CASE("ComputeClockLayout hide_leading_zero blanks tens for midnight") {
  const auto l = btclock::ComputeClockLayout(true, 0, 0, 5, true);
  CHECK(l.digits[0] == ' ');
  CHECK(l.digits[1] == '0');
  CHECK(l.digits[2] == ':');
  CHECK(l.digits[3] == '0');
  CHECK(l.digits[4] == '0');
}

TEST_CASE("ComputeClockLayout hide_leading_zero keeps minute zero-pad") {
  // Minute stays "05", never "5" — matches the user-visible "7:05"
  // example (minute leading zero is always preserved).
  const auto l = btclock::ComputeClockLayout(true, 7, 5, 5, true);
  CHECK(l.digits[0] == ' ');
  CHECK(l.digits[1] == '7');
  CHECK(l.digits[2] == ':');
  CHECK(l.digits[3] == '0');
  CHECK(l.digits[4] == '5');
}

TEST_CASE(
    "ComputeClockLayout hide_leading_zero untouched for two-digit hours") {
  // Noon: 12:00 stays 12:00 with or without the flag.
  const auto noon_hide = btclock::ComputeClockLayout(true, 12, 0, 5, true);
  CHECK(noon_hide.digits[0] == '1');
  CHECK(noon_hide.digits[1] == '2');
  const auto one_pm = btclock::ComputeClockLayout(true, 13, 0, 5, true);
  CHECK(one_pm.digits[0] == '1');
  CHECK(one_pm.digits[1] == '3');
  const auto late = btclock::ComputeClockLayout(true, 23, 45, 5, true);
  CHECK(late.digits[0] == '2');
  CHECK(late.digits[1] == '3');
  CHECK(late.digits[3] == '4');
  CHECK(late.digits[4] == '5');
}

TEST_CASE("ComputeClockLayout hide_leading_zero off keeps legacy HH:MM") {
  // Default (flag=false) must match the pre-refactor output byte-for-byte.
  const auto l = btclock::ComputeClockLayout(true, 7, 0, 5, false);
  CHECK(l.digits[0] == '0');
  CHECK(l.digits[1] == '7');
  CHECK(l.digits[2] == ':');
  CHECK(l.digits[3] == '0');
  CHECK(l.digits[4] == '0');
}

TEST_CASE("ComputeClockLayout hide_leading_zero preserves invalid-blank path") {
  // When NTP hasn't synced the whole layout stays blank regardless.
  const auto l = btclock::ComputeClockLayout(false, 7, 0, 5, true);
  for (int i = 0; i < 5; ++i) CHECK(l.digits[i] == ' ');
}

TEST_CASE("SmallCharsGroups — USD market cap 1.568T across 6 panels") {
  // Reproduces the live Rev B bug: cap = 1,567,956,610,000 (13 digits).
  // Old single-digit-per-panel truncated to the last 6 digits; the
  // correct layout puts 3-digit groups in the trailing panels with a
  // currency separator " $ " just ahead.
  const auto out = btclock::SmallCharsGroups(1'567'956'610'000ULL, " $ ", 6);
  REQUIRE(out.size() == 6);
  CHECK(out[0] == " $ ");
  CHECK(out[1] == "  1");
  CHECK(out[2] == "567");
  CHECK(out[3] == "956");
  CHECK(out[4] == "610");
  CHECK(out[5] == "000");
}

TEST_CASE("SmallCharsGroups — no currency separator emits a blank filler") {
  // Bitcoin Supply uses the same layout but with an empty ccy_cell;
  // the separator slot then gets a single space so the visual gap is
  // preserved.
  const auto out = btclock::SmallCharsGroups(19'875'019ULL, "", 6);
  REQUIRE(out.size() == 6);
  // 19,875,019 → 8 digits → 3 groups, one sep, two blanks ahead.
  CHECK(out[0].empty());
  CHECK(out[1].empty());
  CHECK(out[2] == " ");
  CHECK(out[3] == " 19");
  CHECK(out[4] == "875");
  CHECK(out[5] == "019");
}

TEST_CASE("SmallCharsGroups — exact fit with separator slot") {
  // 9-digit value + separator = 4 cells → fits a 4-slot panel exactly.
  const auto out = btclock::SmallCharsGroups(123'456'789ULL, " $ ", 4);
  REQUIRE(out.size() == 4);
  CHECK(out[0] == " $ ");
  CHECK(out[1] == "123");
  CHECK(out[2] == "456");
  CHECK(out[3] == "789");
}

// ----- LayoutMiningPoolEarnings -----
//
// These cases pin the formatter against silent regressions: the on-device
// renderer and the /api/status mirror both call this helper, so a wrong
// output here paints wrong pixels AND lies to the WebUI. Earlier versions
// of the mining-pool screen allocated only 4 digit slots on a 7-panel
// board; "50.0K" then got left-truncated to "0.0K" on the EPD — caught
// at device-review time rather than here. After Bugs 2+3 freed panel 1
// for digits, 5 slots are available and "50.0K" fits verbatim; we still
// pin the formatter itself so a future renderer tweak can't reintroduce
// the same class of bug.

TEST_CASE("LayoutMiningPoolEarnings — zero sats renders as plain '0'") {
  // Pleb-miner tier: ≤4-digit sats land verbatim. Zero is the explicit
  // "got data, no earnings yet" case (distinct from `daily_sats=nullopt`).
  const auto l = btclock::LayoutMiningPoolEarnings(0);
  CHECK(l.valid);
  CHECK(l.value == "0");
  CHECK(l.unit_label == "SATS");
}

TEST_CASE("LayoutMiningPoolEarnings — single-digit sats verbatim") {
  const auto one = btclock::LayoutMiningPoolEarnings(1);
  CHECK(one.valid);
  CHECK(one.value == "1");
  CHECK(one.unit_label == "SATS");
  const auto nine = btclock::LayoutMiningPoolEarnings(9);
  CHECK(nine.value == "9");
}

TEST_CASE("LayoutMiningPoolEarnings — two/three-digit sats verbatim") {
  CHECK(btclock::LayoutMiningPoolEarnings(99).value == "99");
  CHECK(btclock::LayoutMiningPoolEarnings(100).value == "100");
  CHECK(btclock::LayoutMiningPoolEarnings(999).value == "999");
}

TEST_CASE(
    "LayoutMiningPoolEarnings — 1K..9K sats render verbatim (v3 convention)") {
  // v3's parseMiningPoolStatsDailyEarnings has no 1K..9.99K branch — plebs
  // with 1_000..9_999 sats/day see the raw four-digit number. We keep that
  // so parity tests against old-firmware fixtures stay clean.
  CHECK(btclock::LayoutMiningPoolEarnings(1000).value == "1000");
  CHECK(btclock::LayoutMiningPoolEarnings(5000).value == "5000");
  CHECK(btclock::LayoutMiningPoolEarnings(9999).value == "9999");
}

TEST_CASE("LayoutMiningPoolEarnings — 10K..99K gets one-decimal K suffix") {
  // Lower bound of the K-suffix branch. "10.0K" = 5 chars, previously too
  // wide for the 4-slot digit area; with panel-1 freed this fits verbatim.
  CHECK(btclock::LayoutMiningPoolEarnings(10000).value == "10.0K");
  CHECK(btclock::LayoutMiningPoolEarnings(10000).unit_label == "SATS");
  // 50_000 sats/day — the user-visible bug ("0.0K") before the fix, pinned
  // here so a regression shows up in tests rather than on the display.
  CHECK(btclock::LayoutMiningPoolEarnings(50000).value == "50.0K");
  // Upper bound.
  CHECK(btclock::LayoutMiningPoolEarnings(99999).value == "99.9K");
}

TEST_CASE("LayoutMiningPoolEarnings — 100K..999K drops the decimal") {
  CHECK(btclock::LayoutMiningPoolEarnings(100000).value == "100K");
  CHECK(btclock::LayoutMiningPoolEarnings(500000).value == "500K");
  CHECK(btclock::LayoutMiningPoolEarnings(999999).value == "999K");
}

TEST_CASE("LayoutMiningPoolEarnings — 1M..9.99M gets two decimals") {
  CHECK(btclock::LayoutMiningPoolEarnings(1000000).value == "1.00M");
  CHECK(btclock::LayoutMiningPoolEarnings(1500000).value == "1.50M");
  CHECK(btclock::LayoutMiningPoolEarnings(9999999).value == "9.99M");
}

TEST_CASE("LayoutMiningPoolEarnings — 10M..99.9M gets one decimal") {
  CHECK(btclock::LayoutMiningPoolEarnings(10000000).value == "10.0M");
  CHECK(btclock::LayoutMiningPoolEarnings(25300000).value == "25.3M");
  CHECK(btclock::LayoutMiningPoolEarnings(99999999).value == "99.9M");
}

TEST_CASE("LayoutMiningPoolEarnings — ≥1BTC/day switches to BTC label") {
  // Whale tier: 1 BTC = 100_000_000 sats. Drop to integer-BTC display.
  auto l = btclock::LayoutMiningPoolEarnings(100000000LL);
  CHECK(l.valid);
  CHECK(l.value == "1");
  CHECK(l.unit_label == "BTC");
  // 2.5 BTC/day: integer-BTC truncation matches v3.
  l = btclock::LayoutMiningPoolEarnings(250000000LL);
  CHECK(l.value == "2");
  CHECK(l.unit_label == "BTC");
}

TEST_CASE("LayoutMiningPoolEarnings — missing/negative daily_sats is invalid") {
  // `-1` is the nullopt sentinel the renderer feeds in on missing data.
  const auto l = btclock::LayoutMiningPoolEarnings(-1);
  CHECK_FALSE(l.valid);
}

TEST_CASE("LayoutMiningPoolEarnings — int64_t max doesn't UB") {
  // 9.22e18 sats = 92_233_720_368 BTC/day — far beyond any realistic
  // value. Just make sure the formatter terminates with a BTC label and
  // a non-empty integer string; the renderer truncates to its digit
  // slots. Previous overflow guard would have tripped division-by-zero
  // (etc.) for an unchecked int64 max.
  const int64_t v = std::numeric_limits<int64_t>::max();
  const auto l = btclock::LayoutMiningPoolEarnings(v);
  CHECK(l.valid);
  CHECK(l.unit_label == "BTC");
  CHECK_FALSE(l.value.empty());
}
