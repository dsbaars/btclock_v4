// Host tests for the SSE frame formatter. The frame contract is
// visible to three audiences:
//   1) Browser EventSource parsers (WHATWG spec).
//   2) `curl -N` smoke tests during bring-up.
//   3) Any reverse proxy sitting in front of the device.
// A regression here is user-visible in any of those paths, so the
// tests nail down the exact bytes on the wire.

#include "doctest.h"

#include "sse_frame.hpp"

using btclock::MakeSseComment;
using btclock::MakeSseFrame;

TEST_CASE("MakeSseFrame named event with simple payload") {
  CHECK(MakeSseFrame("status", "{\"ok\":true}") ==
        "event: status\ndata: {\"ok\":true}\n\n");
}

TEST_CASE("MakeSseFrame anonymous (empty name) drops the event line") {
  // The EventSource spec treats missing `event:` as `message`. Our
  // formatter must not emit `event:\n` for the empty-name case — that
  // would be parsed as an empty *event name* on the client, which then
  // dispatches as `message` anyway but looks wrong in traces.
  CHECK(MakeSseFrame("", "hello") == "data: hello\n\n");
}

TEST_CASE("MakeSseFrame empty data produces a bare data: line") {
  // An empty `data:` is a valid SSE frame — used by some servers as a
  // poke to kick EventSource reconnect logic. Keep the output minimal
  // so the frame terminator (blank line) still lands.
  CHECK(MakeSseFrame("ping", "") == "event: ping\ndata:\n\n");
}

TEST_CASE("MakeSseFrame empty name AND data") {
  CHECK(MakeSseFrame("", "") == "data:\n\n");
}

TEST_CASE("MakeSseFrame splits multi-line payload into per-line data fields") {
  // The WHATWG parser joins consecutive `data:` lines with '\n' on
  // the client side, so a literal newline in the payload must be
  // re-emitted as a new `data:` line for the round-trip to be stable.
  CHECK(MakeSseFrame("log", "line1\nline2\nline3") ==
        "event: log\ndata: line1\ndata: line2\ndata: line3\n\n");
}

TEST_CASE("MakeSseFrame trailing newline yields trailing empty data field") {
  // N '\n' in the source -> N+1 `data:` lines on the wire. This is
  // pedantic but matches the spec; the browser will join them back
  // into the original string with an embedded trailing '\n'.
  CHECK(MakeSseFrame("status", "a\n") ==
        "event: status\ndata: a\ndata:\n\n");
}

TEST_CASE("MakeSseFrame handles a payload that's purely a newline") {
  // Edge: a single '\n' splits into two empty data fields.
  CHECK(MakeSseFrame("", "\n") == "data:\ndata:\n\n");
}

TEST_CASE("MakeSseFrame preserves embedded JSON without escaping") {
  // SSE doesn't care about JSON; the server just copies bytes.
  // Quotes, braces and Unicode should pass through unchanged so the
  // client's JSON.parse() round-trips the original payload.
  const std::string payload = R"({"c":"EUR","v":42,"nested":{"ok":true}})";
  const std::string expected =
      "event: status\ndata: " + payload + "\n\n";
  CHECK(MakeSseFrame("status", payload) == expected);
}

TEST_CASE("MakeSseFrame handles CR without a matching LF as a literal byte") {
  // We intentionally only split on '\n'. A lone '\r' stays in the
  // data — the spec says both LF and CRLF terminate a field, but our
  // server-side sources never emit a bare CR, so keeping the
  // formatter simpler is fine. Ensure the behaviour is nailed down.
  CHECK(MakeSseFrame("x", "a\rb") == "event: x\ndata: a\rb\n\n");
}

TEST_CASE("MakeSseComment produces a valid keep-alive frame") {
  // Per the spec, a line starting with ':' is a comment and ignored
  // by the browser. The blank-line terminator still matters: without
  // it, nginx-style proxies may buffer the bytes indefinitely.
  CHECK(MakeSseComment("ping") == ": ping\n\n");
}

TEST_CASE("MakeSseComment with empty text") {
  CHECK(MakeSseComment("") == ":\n\n");
}
