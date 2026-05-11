#include "control_validators.hpp"
#include "doctest.h"

TEST_CASE("IsValidWifiTxPower accepts documented range") {
  // ESP-IDF's esp_wifi_set_max_tx_power takes quarter-dBm. We allow
  // [8, 80]; verify both endpoints + a mid-range value.
  CHECK(btclock::IsValidWifiTxPower(8));
  CHECK(btclock::IsValidWifiTxPower(44));
  CHECK(btclock::IsValidWifiTxPower(78));
  CHECK(btclock::IsValidWifiTxPower(80));
}

TEST_CASE("IsValidWifiTxPower rejects out-of-range values") {
  CHECK_FALSE(btclock::IsValidWifiTxPower(7));
  CHECK_FALSE(btclock::IsValidWifiTxPower(81));
  CHECK_FALSE(btclock::IsValidWifiTxPower(0));
  CHECK_FALSE(btclock::IsValidWifiTxPower(-10));
  CHECK_FALSE(btclock::IsValidWifiTxPower(255));
}

TEST_CASE("IsAcceptableBodySize: rejects empty bodies and oversize bodies") {
  // The ReadFullBody helper and the per-handler kMaxBody gates rely
  // on this contract: 0-length bodies are rejected (handler returns
  // 400, not a confused half-read), and any content_len strictly
  // larger than max_bytes is rejected.
  CHECK_FALSE(btclock::IsAcceptableBodySize(0, 128));
  CHECK_FALSE(btclock::IsAcceptableBodySize(129, 128));
  CHECK_FALSE(btclock::IsAcceptableBodySize(16 * 1024 + 1, 16 * 1024));
}

TEST_CASE("IsAcceptableBodySize: closed upper bound — exactly max_bytes is OK") {
  // Closed-bound contract — the per-handler buffer (e.g. char body[kMaxBody+1])
  // is sized to fit exactly kMaxBody payload bytes plus a NUL.
  CHECK(btclock::IsAcceptableBodySize(1, 128));
  CHECK(btclock::IsAcceptableBodySize(128, 128));
  CHECK(btclock::IsAcceptableBodySize(16 * 1024, 16 * 1024));
}

TEST_CASE("IsAcceptableBodySize: max_bytes=0 rejects every body") {
  // Defensive: a handler that mis-configures kMaxBody to 0 (e.g. a
  // copy-paste regression) should reject everything rather than open
  // an unbounded read.
  CHECK_FALSE(btclock::IsAcceptableBodySize(0, 0));
  CHECK_FALSE(btclock::IsAcceptableBodySize(1, 0));
  CHECK_FALSE(btclock::IsAcceptableBodySize(1024, 0));
}
