// Pure-logic helpers used by the control server. Kept in their own
// header (no IDF includes) so the host-test suite can link against
// them directly without pulling esp_http_server into the test binary.

#pragma once

#include <cstddef>

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

// Body-size gate shared by every JSON POST/PATCH handler. We reject
// empty bodies (no Content-Length / Content-Length: 0) AND oversize
// bodies because the heap allocations downstream sit on a tight PSRAM
// budget and a missing Content-Length normally means the client lied
// about the request shape — better a fast 400 than a half-read body.
// The closed upper bound (<=) is the contract the per-handler
// kMaxBody constants depend on; do not change to strict-less-than
// without auditing every call site.
inline bool IsAcceptableBodySize(std::size_t content_len,
                                 std::size_t max_bytes) {
  return content_len > 0 && content_len <= max_bytes;
}

}  // namespace btclock
