// Host tests for the block-fee-rate screen layout.
//
// Exercises the pure-logic helper in main/screens/fee_rate_layout.hpp —
// the header is header-only and doesn't include any ESP-IDF APIs, so we
// can include it directly without the framebuffer / EPD dependency.

#include "doctest.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "screens/fee_rate_layout.hpp"

namespace {

constexpr size_t kPanels7 = 7;  // Rev A / Rev B / legacy
constexpr size_t kPanels8 = 8;  // V8 (dual MCP23017)
constexpr size_t kDigits7 = btclock::kFeeRateDigitPanels<kPanels7>;
constexpr size_t kDigits8 = btclock::kFeeRateDigitPanels<kPanels8>;

template <size_t Slots>
std::string Render(const std::array<char, Slots>& d) {
  return std::string(d.data(), Slots);
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

TEST_CASE("fee=1 right-justifies with blanks on 7-panel board") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1, d);
  // Six digit panels (N=7, label takes slot 0), so expect 5 spaces + '1'.
  CHECK(Render(d) == "     1");
}

TEST_CASE("fee=1 right-justifies with blanks on 8-panel board") {
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(1, d);
  CHECK(Render(d) == "      1");
}

TEST_CASE("fee=1234 right-justifies with partial blank padding") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1234, d);
  CHECK(Render(d) == "  1234");
}

TEST_CASE("fee=1234 exactly fills the digit panels on V8 too") {
  std::array<char, kDigits8> d;
  btclock::LayoutFeeRate(1234, d);
  CHECK(Render(d) == "   1234");
}

TEST_CASE("fee=-1 paints all blanks (pre-data state)") {
  std::array<char, kDigits7> d7;
  btclock::LayoutFeeRate(-1, d7);
  CHECK(Render(d7) == "      ");

  std::array<char, kDigits8> d8;
  btclock::LayoutFeeRate(-1, d8);
  CHECK(Render(d8) == "       ");
}

TEST_CASE("fee=0 renders a single '0' (legitimate 'no fee' value)") {
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(0, d);
  CHECK(Render(d) == "     0");
}

TEST_CASE("huge fee gets left-truncated rather than overflowing") {
  // Wider than 6 digits — sats/vB will never realistically go here, but
  // guard against UB if the server ever pushes something weird.
  std::array<char, kDigits7> d;
  btclock::LayoutFeeRate(1234567, d);
  // Oldest digit drops off on the left; the last 6 remain.
  CHECK(Render(d) == "234567");
}

TEST_CASE("partial-refresh diff: no change → every slot is false") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(42, now);
  btclock::LayoutFeeRate(42, before);
  const auto update = btclock::DiffFeeRateDigits(now, before, /*full=*/false);
  for (size_t i = 0; i < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == false);
  }
}

TEST_CASE("partial-refresh diff: only changed digit slots flag true") {
  // 42 → 43: layout is "    42" → "    43". Only the last slot changes.
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(43, now);
  btclock::LayoutFeeRate(42, before);
  const auto update = btclock::DiffFeeRateDigits(now, before, /*full=*/false);
  for (size_t i = 0; i + 1 < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == false);
  }
  CHECK(update[update.size() - 1] == true);
}

TEST_CASE("partial-refresh diff: a more-digits transition flags both") {
  // 9 → 10: "     9" → "    10". Last two slots change, others don't.
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(10, now);
  btclock::LayoutFeeRate(9, before);
  const auto update = btclock::DiffFeeRateDigits(now, before, /*full=*/false);
  CHECK(update[0] == false);
  CHECK(update[1] == false);
  CHECK(update[2] == false);
  CHECK(update[3] == false);
  CHECK(update[4] == true);
  CHECK(update[5] == true);
}

TEST_CASE("partial-refresh diff: full_refresh=true forces every slot") {
  std::array<char, kDigits7> now, before;
  btclock::LayoutFeeRate(42, now);
  btclock::LayoutFeeRate(42, before);
  const auto update = btclock::DiffFeeRateDigits(now, before, /*full=*/true);
  for (size_t i = 0; i < update.size(); ++i) {
    CAPTURE(i);
    CHECK(update[i] == true);
  }
}

// --- kDigitRef guard for fee_rate.cpp ---
// Mirrors the regression test in test_screen_ref_chars.cpp; re-stated
// here so an inline punct-ref in fee_rate.cpp fails early. Keep in sync
// with the list in test_screen_ref_chars.cpp if more renderers land.
TEST_CASE("fee_rate.cpp does not inline a punct-inclusive digit ref") {
  const std::string src = ReadFile("main/screens/fee_rate.cpp");
  CHECK(src.find("kDigitAndPuncRef") == std::string::npos);
  CHECK(src.find("\"0123456789.\"") == std::string::npos);
  CHECK(src.find("\"0123456789,\"") == std::string::npos);
  CHECK(src.find("\"0123456789.,:\"") == std::string::npos);
  CHECK(src.find("\"0123456789.,\"") == std::string::npos);
}
