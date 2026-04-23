#include "doctest.h"

#include "control_validators.hpp"

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
