// Pins the URL-equality gate that decides whether the zap listener
// rides the Nostr data source's existing WSS connection (NIP-01 multi-
// subscription on one socket) or opens a second RelayClient.
//
// Sharing collapses ~30+ KB of internal SRAM (a second 12 KB WS task
// stack + 8 KB rx buffer + mbedTLS context per WSS) and the matching
// largest-block fragmentation that pinned `espLargestFreeBlock` at 7 KB
// and silently broke the EPD render path on long-uptime devices. The
// runtime A/B in bd btclock_v4-17r confirmed the fragmentation cause;
// this test guards the decision logic so a future refactor can't
// regress it back to "always open a second WSS".
//
// The helper is intentionally narrow. Wide URL normalisation (port
// canonicalisation, path collapsing, percent-decoding) is a footgun
// because the underlying esp_websocket_client treats two URLs that
// "look the same" but differ at the byte level as different sockets.
// Anything we collapse here MUST also collapse inside that lib, or
// we'd share a manager bound to a different connection. Keep
// normalisation to the rules that are safe under that constraint:
// case-insensitive ASCII compare + a single trailing-slash strip.

#include "app/boot/init_zap_listener.hpp"
#include "doctest.h"

TEST_CASE("ShouldShareNostrRelay: identical URLs match") {
  // The common case — most users point both nostrRelay (the data
  // source) and the zap listener at the same single relay.
  CHECK(btclock::ShouldShareNostrRelay("wss://relay.primal.net",
                                       "wss://relay.primal.net"));
}

TEST_CASE("ShouldShareNostrRelay: trailing slash is normalised") {
  // Some configurators emit the relay root with a trailing slash, some
  // without. Both should share — the WS lib treats them as one
  // connection (the path component is collapsed at the HTTP-Upgrade
  // step).
  CHECK(btclock::ShouldShareNostrRelay("wss://relay.primal.net/",
                                       "wss://relay.primal.net"));
  CHECK(btclock::ShouldShareNostrRelay("wss://relay.primal.net",
                                       "wss://relay.primal.net/"));
  CHECK(btclock::ShouldShareNostrRelay("wss://relay.primal.net/",
                                       "wss://relay.primal.net/"));
}

TEST_CASE("ShouldShareNostrRelay: case-insensitive scheme + host") {
  // RFC 3986 §3.1 (scheme) + §3.2.2 (host) are case-insensitive. The
  // WS lib lowercases internally before resolving the socket, so two
  // URLs that differ only in case bind to the same connection.
  CHECK(btclock::ShouldShareNostrRelay("WSS://Relay.Primal.NET",
                                       "wss://relay.primal.net"));
  CHECK(btclock::ShouldShareNostrRelay("wss://RELAY.PRIMAL.NET/",
                                       "wss://relay.primal.net"));
}

TEST_CASE("ShouldShareNostrRelay: different relays do not share") {
  // The whole point of a separate zap listener: the user can route
  // notifications through a different (often more reliable) relay
  // than the bulky data feed. A false positive here would silently
  // bind the zap subscription to the wrong WSS.
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://relay.primal.net",
                                             "wss://relay.damus.io"));
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://relay.primal.net",
                                             "wss://relay.snort.social"));
}

TEST_CASE("ShouldShareNostrRelay: scheme mismatch refuses sharing") {
  // ws:// vs wss:// resolve to different sockets even when the host
  // matches. The TLS context isn't reusable between them, and the
  // user's choice of plaintext vs TLS is meaningful (often an
  // air-gapped local relay vs a public WSS).
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://relay.example.com",
                                             "ws://relay.example.com"));
}

TEST_CASE("ShouldShareNostrRelay: empty inputs never share") {
  // Either URL absent → there's no live socket to ride. The caller
  // must fall back to the dedicated-WSS path (or, when the data
  // source URL is empty, that means dataSource != 2 and there's no
  // shared connection candidate at all).
  CHECK_FALSE(btclock::ShouldShareNostrRelay("", "wss://relay.example.com"));
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://relay.example.com", ""));
  CHECK_FALSE(btclock::ShouldShareNostrRelay("", ""));
}

TEST_CASE("ShouldShareNostrRelay: trailing slash on one side only") {
  // The single-trailing-slash strip is symmetric — covering this
  // explicitly so a future "normalise more aggressively" change can't
  // accidentally treat "wss://r/" and "wss://r//" as equal too. Two
  // trailing slashes is a degenerate URL we don't normalise.
  CHECK(btclock::ShouldShareNostrRelay("wss://r.example/", "wss://r.example"));
  CHECK_FALSE(
      btclock::ShouldShareNostrRelay("wss://r.example//", "wss://r.example"));
}

TEST_CASE("ShouldShareNostrRelay: query strings differ, no share") {
  // Some relays accept a `?token=` for authentication. Two URLs that
  // differ only in query string are different sockets to the WS lib
  // (path is sent verbatim in the HTTP Upgrade). Sharing would bind
  // the zap subscription to a connection the user didn't authenticate.
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://r.example/?a=1",
                                             "wss://r.example/?a=2"));
  CHECK_FALSE(btclock::ShouldShareNostrRelay("wss://r.example/?a=1",
                                             "wss://r.example"));
}
