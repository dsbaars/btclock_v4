// Pin BuildBtclockSourceUri's mapping from the v3 dataSource enum +
// ceEndpoint + ceDisableSSL onto an actual WSS URL.
//
// These three settings are boot_only — the firmware reads them once in
// WireDataSources and feeds the result to BtclockDataSource. Bugs here
// silently mean "the device keeps talking to ws.btclock.dev no matter
// what you PATCH", which is exactly the regression this test guards
// against.

#include <cstdint>
#include <string>

#include "doctest.h"
#include "sources/sources.hpp"

TEST_CASE("BuildBtclockSourceUri: dataSource=0 -> public default") {
  CHECK(btclock::BuildBtclockSourceUri(0, "ignored", false) ==
        "wss://ws.btclock.dev/api/v2/ws");
  // ceDisableSSL must NOT downgrade the public default — that endpoint
  // only speaks TLS, and an http downgrade would just hang the boot.
  CHECK(btclock::BuildBtclockSourceUri(0, "ignored", true) ==
        "wss://ws.btclock.dev/api/v2/ws");
}

TEST_CASE("BuildBtclockSourceUri: dataSource=2 honours ceEndpoint + scheme") {
  CHECK(btclock::BuildBtclockSourceUri(2, "ws-testing.btclock.dev", false) ==
        "wss://ws-testing.btclock.dev/api/v2/ws");
  CHECK(btclock::BuildBtclockSourceUri(2, "ws-testing.btclock.dev", true) ==
        "ws://ws-testing.btclock.dev/api/v2/ws");
}

TEST_CASE("BuildBtclockSourceUri: dataSource=2 strips a leading scheme") {
  // Users routinely paste a full URL into the textbox. The scheme must
  // come from ceDisableSSL, not from whatever the user typed, otherwise
  // the two settings can disagree.
  CHECK(btclock::BuildBtclockSourceUri(2, "wss://ws-testing.btclock.dev",
                                       false) ==
        "wss://ws-testing.btclock.dev/api/v2/ws");
  CHECK(
      btclock::BuildBtclockSourceUri(2, "ws://ws-testing.btclock.dev", true) ==
      "ws://ws-testing.btclock.dev/api/v2/ws");
}

TEST_CASE("BuildBtclockSourceUri: dataSource=2 keeps a trailing slash") {
  // A trailing slash on the host part is harmless ("//api/v2/ws") for
  // the ESP-IDF websocket client but ugly. We document the current
  // behaviour rather than try to clean every quirk: only the leading
  // scheme is stripped.
  CHECK(btclock::BuildBtclockSourceUri(2, "wss://ws-testing.btclock.dev/",
                                       false) ==
        "wss://ws-testing.btclock.dev//api/v2/ws");
}

TEST_CASE(
    "BuildBtclockSourceUri: dataSource=2 with empty endpoint -> default") {
  // Empty string would produce "wss:///api/v2/ws" which is unroutable.
  // Falling back to the public default keeps the device functional.
  CHECK(btclock::BuildBtclockSourceUri(2, "", false) ==
        "wss://ws.btclock.dev/api/v2/ws");
}

TEST_CASE("BuildBtclockSourceUri: dataSource=1 falls back to default") {
  // mempool+kraken (THIRD_PARTY_SOURCE) is handled in WireDataSources via
  // its own source class with hard-coded URIs; this helper is only called
  // on the v2/ws path, so ds=1 should never round-trip through here. If
  // it does, fall back to public so the device stays connected somewhere.
  CHECK(btclock::BuildBtclockSourceUri(1, "ignored", false) ==
        "wss://ws.btclock.dev/api/v2/ws");
}

TEST_CASE(
    "BuildBtclockSourceUri: dataSource=3 (WebUI CUSTOM) honours ceEndpoint") {
  // WebUI's DataSourceType.CUSTOM_SOURCE = 3
  // (data/src/lib/types/settings.ts). Used to fall through to the public
  // default — which silently dropped users on a LAN endpoint back to
  // ws.btclock.dev. Now shares the custom branch with ds=2.
  CHECK(btclock::BuildBtclockSourceUri(3, "192.168.21.88:8080", true) ==
        "ws://192.168.21.88:8080/api/v2/ws");
  CHECK(btclock::BuildBtclockSourceUri(3, "ws-testing.btclock.dev", false) ==
        "wss://ws-testing.btclock.dev/api/v2/ws");
}

TEST_CASE(
    "BuildBtclockSourceUri: dataSource=3 with empty endpoint -> default") {
  CHECK(btclock::BuildBtclockSourceUri(3, "", false) ==
        "wss://ws.btclock.dev/api/v2/ws");
  CHECK(btclock::BuildBtclockSourceUri(3, "", true) ==
        "wss://ws.btclock.dev/api/v2/ws");
}
