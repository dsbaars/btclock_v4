// Pin BuildLastblockUri's mapping from the WS URI to the matching
// HTTP /api/lastblock URL. The stale-block watchdog uses this to probe
// the same backend the websocket talks to — wrong derivation here means
// the recovery path silently 404s for users on a custom endpoint, or
// (worse) calls a different host entirely.

#include <string>

#include "btclock_lastblock_uri.hpp"
#include "doctest.h"

TEST_CASE("BuildLastblockUri: wss public default") {
  CHECK(btclock::BuildLastblockUri("wss://ws.btclock.dev/api/v2/ws") ==
        "https://ws.btclock.dev/api/lastblock");
}

TEST_CASE("BuildLastblockUri: ws preserves plaintext scheme") {
  CHECK(btclock::BuildLastblockUri("ws://ws-testing.btclock.dev/api/v2/ws") ==
        "http://ws-testing.btclock.dev/api/lastblock");
  CHECK(btclock::BuildLastblockUri("wss://ws-testing.btclock.dev/api/v2/ws") ==
        "https://ws-testing.btclock.dev/api/lastblock");
}

TEST_CASE("BuildLastblockUri: explicit port is preserved on the authority") {
  CHECK(btclock::BuildLastblockUri("ws://192.168.1.50:8080/api/v2/ws") ==
        "http://192.168.1.50:8080/api/lastblock");
}

TEST_CASE("BuildLastblockUri: trailing path beyond /api/v2/ws is dropped") {
  // A relay rename to /api/v3/ws shouldn't smuggle "v3" into the
  // lastblock path — keep the probe pinned at /api/lastblock.
  CHECK(btclock::BuildLastblockUri("wss://ws.example.com/api/v3/ws") ==
        "https://ws.example.com/api/lastblock");
}

TEST_CASE("BuildLastblockUri: rejects non-ws schemes") {
  CHECK(btclock::BuildLastblockUri("https://ws.btclock.dev/api/lastblock") ==
        "");
  CHECK(btclock::BuildLastblockUri("") == "");
  CHECK(btclock::BuildLastblockUri("ws.btclock.dev") == "");
}

TEST_CASE("BuildLastblockUri: empty authority refuses to build") {
  // Would otherwise produce "https:///api/lastblock" — unroutable.
  CHECK(btclock::BuildLastblockUri("wss:///api/v2/ws") == "");
  CHECK(btclock::BuildLastblockUri("ws:///") == "");
}
