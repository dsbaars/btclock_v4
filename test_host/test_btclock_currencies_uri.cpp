// Pin BuildCurrenciesUri's mapping from the WS URI to the matching
// HTTP /api/v2/currencies URL. The boot fetcher uses this to probe the
// same backend the websocket talks to — wrong derivation here means the
// available-currency catalogue silently 404s for users on a custom
// endpoint, or (worse) calls a different host entirely.

#include <string>

#include "btclock_currencies_uri.hpp"
#include "doctest.h"

TEST_CASE("BuildCurrenciesUri: wss public default") {
  CHECK(btclock::BuildCurrenciesUri("wss://ws.btclock.dev/api/v2/ws") ==
        "https://ws.btclock.dev/api/v2/currencies");
}

TEST_CASE("BuildCurrenciesUri: ws preserves plaintext scheme") {
  CHECK(btclock::BuildCurrenciesUri("ws://ws-testing.btclock.dev/api/v2/ws") ==
        "http://ws-testing.btclock.dev/api/v2/currencies");
  CHECK(btclock::BuildCurrenciesUri("wss://ws-testing.btclock.dev/api/v2/ws") ==
        "https://ws-testing.btclock.dev/api/v2/currencies");
}

TEST_CASE("BuildCurrenciesUri: explicit port is preserved on the authority") {
  CHECK(btclock::BuildCurrenciesUri("ws://192.168.1.50:8080/api/v2/ws") ==
        "http://192.168.1.50:8080/api/v2/currencies");
}

TEST_CASE("BuildCurrenciesUri: trailing path beyond /api/v2/ws is dropped") {
  // A relay rename to /api/v3/ws shouldn't smuggle "v3" into the
  // currencies path — keep the probe pinned at /api/v2/currencies.
  CHECK(btclock::BuildCurrenciesUri("wss://ws.example.com/api/v3/ws") ==
        "https://ws.example.com/api/v2/currencies");
}

TEST_CASE("BuildCurrenciesUri: rejects non-ws schemes") {
  CHECK(btclock::BuildCurrenciesUri(
            "https://ws.btclock.dev/api/v2/currencies") == "");
  CHECK(btclock::BuildCurrenciesUri("") == "");
  CHECK(btclock::BuildCurrenciesUri("ws.btclock.dev") == "");
}

TEST_CASE("BuildCurrenciesUri: empty authority refuses to build") {
  // Would otherwise produce "https:///api/v2/currencies" — unroutable.
  CHECK(btclock::BuildCurrenciesUri("wss:///api/v2/ws") == "");
  CHECK(btclock::BuildCurrenciesUri("ws:///") == "");
}
