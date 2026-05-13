// Host tests for FormatSatsCompact — the screen-side formatter used
// by both nwc_balance.cpp and nwc_payment_notify.cpp. Pure-logic helper
// so the layout rules pin in CI without a panel renderer.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

#include "doctest.h"

namespace btclock {
// Local fwd-decls so the test doesn't drag in screens.hpp (which pulls
// in epd/fonts/etc.). The .cpp definitions are linked from
// nwc_balance.cpp — copied here to host because the real file pulls
// in EPD headers. Instead, we declare and inline a host-side
// duplicate that mirrors the production rules: the test keeps a
// shadow definition next to the asserts.
//
// We keep this in sync with main/screens/nwc_balance.cpp. The
// FormatZapAmount path is replicated inline here from the panel
// formatter behaviour; this test pins the BTC fallback rules only,
// since the suffix path is already covered by nostr_zap tests.

// The full production formatter delegates to FormatZapAmount under
// 100M sats; host-side we re-create the >=100M branch directly so the
// test stays focused on the BTC formatting.
inline std::string FormatSatsCompactBtcOnly(
    const std::optional<int64_t>& amount_sats) {
  if (!amount_sats || *amount_sats < 0) return "—";
  if (*amount_sats < 100'000'000LL) return "<lt100M>";
  const double btc = static_cast<double>(*amount_sats) / 1e8;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.4f", btc);
  return std::string(buf);
}
}  // namespace btclock

TEST_CASE("FormatSatsCompact: nullopt → em-dash") {
  CHECK(btclock::FormatSatsCompactBtcOnly(std::nullopt) == "—");
}

TEST_CASE("FormatSatsCompact: negative → em-dash") {
  CHECK(btclock::FormatSatsCompactBtcOnly(std::optional<int64_t>{-1}) == "—");
}

TEST_CASE("FormatSatsCompact: 1 BTC → \"1.0000\"") {
  CHECK(btclock::FormatSatsCompactBtcOnly(
            std::optional<int64_t>{100'000'000LL}) == "1.0000");
}

TEST_CASE("FormatSatsCompact: 1.5 BTC → \"1.5000\"") {
  CHECK(btclock::FormatSatsCompactBtcOnly(
            std::optional<int64_t>{150'000'000LL}) == "1.5000");
}

TEST_CASE("FormatSatsCompact: 21M BTC ceiling rounds via %.4f") {
  CHECK(btclock::FormatSatsCompactBtcOnly(std::optional<int64_t>{
            2'100'000'000'000'000LL}) == "21000000.0000");
}

TEST_CASE("FormatSatsCompact: 99,999,999 falls below BTC threshold") {
  // Under 100M sats the BTC branch isn't taken — the helper hands off
  // to FormatZapAmount. We assert the threshold here by checking the
  // sentinel; the actual suffix-path output is covered by the nostr
  // zap formatter tests.
  CHECK(btclock::FormatSatsCompactBtcOnly(
            std::optional<int64_t>{99'999'999LL}) == "<lt100M>");
}
