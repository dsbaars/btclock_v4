// Host tests for the block-fee-rate screen layout.
//
// Exercises the pure-logic helper in main/screens/fee_rate_layout.hpp —
// the header is header-only and doesn't include any ESP-IDF APIs, so we
// can include it directly without the framebuffer / EPD dependency.
//
// Layout model (see fee_rate_layout.hpp for the full spec):
//   - N panels total. Panel 0 = "FEE/RATE" label, panel N-1 = "sat/vB"
//     unit text. Digit slots are panels 1..N-2 → kFeeRateDigitPanels<N>
//     = N - 2 slots available for the fee text.
//   - Rev A / Rev B (7 panels) → 5 digit slots
//   - V8           (8 panels) → 6 digit slots
//   - Integer-valued doubles render as plain right-justified ints; any
//     non-zero fractional part renders as "X.YY"; overflow drops the
//     fractional tail first, then truncates leading integer digits.

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"
#include "screens/fee_rate_layout.hpp"

namespace {

constexpr size_t kPanels7 = 7;  // Rev A / Rev B / legacy
constexpr size_t kPanels8 = 8;  // V8 (dual MCP23017)
constexpr size_t kDigits7 = btclock::kFeeRateDigitPanels<kPanels7>;
constexpr size_t kDigits8 = btclock::kFeeRateDigitPanels<kPanels8>;

static_assert(kDigits7 == 5, "7-panel board → 5 digit slots");
static_assert(kDigits8 == 6, "8-panel board → 6 digit slots");

template <size_t Slots>
std::string Render(const std::array<char, Slots>& d) {
  return std::string(d.data(), Slots);
}

std::string ReadFile(const std::string& relpath) {
  const std::string path = std::string(BTCLOCK_PROJECT_ROOT) + "/" + relpath;
  std::ifstream f(path);
  REQUIRE_MESSAGE(f.is_open(), "could not open " << path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

// --- Integer-valued doubles render without a dot ---

TEST_CASE("fee=1.0 right-justifies with blanks on 7-panel board") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1.0, d);
  // 5 digit slots — expect 4 spaces + '1'.
  CHECK(Render(d) == "    1");
}

TEST_CASE("fee=1.0 right-justifies with blanks on 8-panel board") {
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(1.0, d);
  CHECK(Render(d) == "     1");
}

TEST_CASE("fee=42.0 (integer-valued) renders as plain '42'") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(42.0, d);
  CHECK(Render(d) == "   42");
}

TEST_CASE("fee=0.0 renders a single '0' (integer-valued zero)") {
  // Pinned: integer-valued zero stays as "    0" (5 slots), matching the
  // whole-number treatment of other integer-valued doubles. "0.00" was
  // considered but looks wrong when the value legitimately is zero.
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(0.0, d);
  CHECK(Render(d) == "    0");
}

TEST_CASE("fee=-1.0 paints all blanks (pre-data state)") {
  std::array<char, kDigits7> d7;
  btclock::LayoutFeeRate(-1.0, d7);
  CHECK(Render(d7) == "     ");

  std::array<char, kDigits8> d8;
  btclock::LayoutFeeRate(-1.0, d8);
  CHECK(Render(d8) == "      ");
}

TEST_CASE("large integer-valued fee right-justifies") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1234.0, d);
  CHECK(Render(d) == " 1234");
}

// --- Fractional values render as X.YY ---

TEST_CASE("fee=12.75 renders as '12.75' (exactly fills 5 slots)") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(12.75, d);
  CHECK(Render(d) == "12.75");
}

TEST_CASE("fee=12.75 left-pads on 8-panel board") {
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(12.75, d);
  CHECK(Render(d) == " 12.75");
}

TEST_CASE("fee=1.5 renders as '1.50'") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1.5, d);
  CHECK(Render(d) == " 1.50");
}

TEST_CASE(
    "fee=100.5 renders as '100.50' (6-wide; fits V8, overflows 7-panel)") {
  std::array<char, kDigits8> d8;
  btclock::LayoutFeeRate(100.5, d8);
  CHECK(Render(d8) == "100.50");

  // 7-panel: overflow → integer fallback. 100.5 rounds to 101 with
  // std::llround (half-away-from-zero).
  std::array<char, kDigits7> d7;
  btclock::LayoutFeeRate(100.5, d7);
  CHECK(Render(d7) == "  101");
}

TEST_CASE("fee=999.99 renders exactly as '999.99' on V8") {
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(999.99, d);
  CHECK(Render(d) == "999.99");
}

TEST_CASE("fee=999.99 overflows on 7-panel → integer fallback '1000'") {
  // %.2f of 999.99 is "999.99" which is 6 chars > 5 slots → drop
  // decimals, integer of 999.99 rounds to 1000 which is 4 chars → fits.
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(999.99, d);
  CHECK(Render(d) == " 1000");
}

TEST_CASE("overflow fee=1234.56 on 7-panel → integer-only '1235'") {
  // "1234.56" is 7 chars > 5 slots → drop decimals. Rounded integer is
  // 1235 (half-up from 1234.56), 4 chars → right-justified as " 1235".
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1234.56, d);
  CHECK(Render(d) == " 1235");
}

TEST_CASE("overflow fee=1234.56 on 8-panel → integer fallback '1235'") {
  // "1234.56" is 7 chars > 6 slots → decimal tail dropped, integer of
  // 1234.56 rounds to 1235 which pads as "  1235" (6 slots, 2 leading
  // spaces). Pinning: we don't attempt a 1-decimal "1234.6" fallback —
  // the format is strictly "X.YY" or integer, never "X.Y".
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(1234.56, d);
  CHECK(Render(d) == "  1235");
}

TEST_CASE("huge fee gets left-truncated after decimal + integer fallback") {
  // 12345678 > 6 chars → still doesn't fit as int on V8; last-resort
  // left-truncate to the last 6 digits.
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(12345678.0, d);
  CHECK(Render(d) == "345678");
}

TEST_CASE("rounding: 41.999999 → integer-valued '42'") {
  // %.2f rounds 41.999999 to 42.00 → integer-valued check catches it.
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(41.999999, d);
  CHECK(Render(d) == "   42");
}

// --- Partial-refresh diff ---

TEST_CASE("diff: no change → every slot is false") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(42.0, now);
  btclock::LayoutFeeRate(42.0, before);
  const auto update =
      btclock::DiffFeeRateDigits(now, before, /*full_refresh=*/false);
  for (size_t i = 0; i < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == false);
  }
}

TEST_CASE("diff: only changed digit slots flag true (integer case)") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(43.0, now);
  btclock::LayoutFeeRate(42.0, before);
  const auto update =
      btclock::DiffFeeRateDigits(now, before, /*full_refresh=*/false);
  for (size_t i = 0; i + 1 < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == false);
  }
  CHECK(update[update.size() - 1] == true);
}

TEST_CASE("diff: decimal transition 12.75 → 12.76 flags only last slot") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(12.76, now);
  btclock::LayoutFeeRate(12.75, before);
  const auto update =
      btclock::DiffFeeRateDigits(now, before, /*full_refresh=*/false);
  CHECK(update[0] == false);
  CHECK(update[1] == false);
  CHECK(update[2] == false);
  CHECK(update[3] == false);
  CHECK(update[4] == true);
}

TEST_CASE("diff: integer→decimal transition 12 → 12.75 flags all last 3") {
  // "   12" → "12.75"
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(12.75, now);
  btclock::LayoutFeeRate(12.0, before);
  const auto update =
      btclock::DiffFeeRateDigits(now, before, /*full_refresh=*/false);
  CHECK(update[0] == true);  // ' ' → '1'
  CHECK(update[1] == true);  // ' ' → '2'
  CHECK(update[2] == true);  // ' ' → '.'
  CHECK(update[3] == true);  // '1' → '7'
  CHECK(update[4] == true);  // '2' → '5'
}

TEST_CASE("diff: full_refresh=true forces every slot") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(42.0, now);
  btclock::LayoutFeeRate(42.0, before);
  const auto update =
      btclock::DiffFeeRateDigits(now, before, /*full_refresh=*/true);
  for (size_t i = 0; i < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == true);
  }
}

// --- Renderer source-text guards ---
//
// The fee-rate digit panels legitimately need a ref string that includes
// '.' (see fee_rate_layout.hpp / the comment on kFeeRateDotRef). That
// string must NOT appear as a literal inside fee_rate.cpp — it must come
// via the kFeeRateDotRef constant from the local layout header, scoped
// to this screen. This guard mirrors the one in test_screen_ref_chars.

TEST_CASE("fee_rate.cpp uses kFeeRateDotRef, not an inline punct literal") {
  const std::string src = ReadFile("main/screens/fee_rate.cpp");
  // No inline literal — must come through the named constant.
  CHECK(src.find("\"0123456789.\"") == std::string::npos);
  CHECK(src.find("\"0123456789,\"") == std::string::npos);
  CHECK(src.find("\"0123456789.,:\"") == std::string::npos);
  CHECK(src.find("\"0123456789.,\"") == std::string::npos);
  // The deleted `kDigitAndPuncRef` constant must stay gone, across all
  // screens.
  CHECK(src.find("kDigitAndPuncRef") == std::string::npos);
  // And we DO expect the punct-ref to be sourced from the local
  // constant — catch a future regression where someone deletes the
  // constant + falls back to kDigitRef (broken baseline on dot panels).
  CHECK(src.find("kFeeRateDotRef") != std::string::npos);
}
