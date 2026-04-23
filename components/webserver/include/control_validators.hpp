// Pure-logic helpers used by the control server. Kept in their own
// header (no IDF includes) so the host-test suite can link against
// them directly without pulling esp_http_server into the test binary.

#pragma once

namespace btclock {

// Validate a WiFi TX-power value (quarter-dBm units, matching the
// esp_wifi_set_max_tx_power API). The ESP-IDF accepts values in the
// closed range [8, 84] per the docs; the old Arduino firmware used an
// enum that tops out around 78 (WIFI_POWER_19_5dBm). We pick [8, 80] —
// wide enough to cover every documented level, tight enough to reject
// obvious junk.
inline bool IsValidWifiTxPower(int raw) {
  return raw >= 8 && raw <= 80;
}

}  // namespace btclock
