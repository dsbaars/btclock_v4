// One-shot HTTP(S) GET on `/api/v2/currencies` for the host derived from
// the WS URI. Returns the codes parsed by ParseCurrenciesJson, or an
// empty vector on any failure (DNS, TLS, non-2xx, malformed body). The
// caller is expected to fall back to its static catalogue on empty.
//
// Bound to esp_http_client + the IDF cert bundle so it lives in the
// firmware build only — host tests pin BuildCurrenciesUri /
// ParseCurrenciesJson directly.

#pragma once

#include <string>
#include <vector>

namespace btclock {

// `ws_uri` is the same URI BtclockDataSource is constructed with
// (ws://… or wss://…). Path is replaced with `/api/v2/currencies`;
// scheme is mapped wss→https / ws→http.
std::vector<std::string> FetchAvailableCurrencies(const std::string& ws_uri);

}  // namespace btclock
