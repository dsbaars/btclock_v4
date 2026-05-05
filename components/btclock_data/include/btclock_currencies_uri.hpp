// Pure helper: derive `https://<host>/api/v2/currencies` from the WS URI
// the data source is configured with. Mirrors btclock_lastblock_uri.hpp:
// kept as its own translation unit so host tests can pin the shape
// without dragging in ESP-IDF / mbedtls. Used at boot to fetch the
// upstream-supported currency catalogue when dataSource is 0 (BTClock)
// or 2 (custom endpoint).

#pragma once

#include <string>

namespace btclock {

// Returns an empty string when the input doesn't look like a ws[s]:// URL —
// caller treats that as "skip the fetch". Otherwise:
//   wss://host[:port]/api/v2/ws -> https://host[:port]/api/v2/currencies
//   ws://host[:port]/api/v2/ws  -> http://host[:port]/api/v2/currencies
// Trailing path beyond the authority is dropped — a future relay rename
// to /api/v3/ws shouldn't smuggle "v3" into the currencies probe.
std::string BuildCurrenciesUri(const std::string& ws_uri);

}  // namespace btclock
