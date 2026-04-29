// Pure helper: derive `https://<host>/api/lastblock` from the WS URI the
// data source is configured with. Split out of the source file so host
// tests can pin the shape without dragging in ESP-IDF / mbedtls. Used by
// the stale-block watchdog to probe the same backend over HTTP when the
// websocket has gone silent.

#pragma once

#include <string>

namespace btclock {

// Returns an empty string when the input doesn't look like a ws[s]:// URL
// — caller treats that as "skip this probe cycle." Otherwise:
//   wss://host[:port]/api/v2/ws -> https://host[:port]/api/lastblock
//   ws://host[:port]/api/v2/ws  -> http://host[:port]/api/lastblock
// Trailing path beyond /api/v2/ws is also stripped (defensive — the
// configured URI is always /api/v2/ws today, but a future endpoint
// rename shouldn't silently route the watchdog at the wrong path).
std::string BuildLastblockUri(const std::string& ws_uri);

}  // namespace btclock
