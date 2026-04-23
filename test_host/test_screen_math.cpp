#include "doctest.h"

#include <cstdint>
#include <cstring>

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
  CHECK(btclock::SupplyAtBlock(33 * 210000 + 1) <=
        btclock::kMaxSupplyBtc);
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
  CHECK(btclock::MarketCap(100000, 840000) ==
        100000ULL * 19687500ULL);
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
