// Host tests for the led_curves hex color parser and formatter.
//
// The WebUI and /api/lights/color POST body both use `#RRGGBB` strings;
// NVS persistence stores them as packed uint32 (0x00RRGGBB) so the
// round-trip parse → format → parse must be stable.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "doctest.h"
#include "io/led_curves.hpp"

using btclock::led_curves::FormatHexColor;
using btclock::led_curves::ParseHexColor;

TEST_CASE("ParseHexColor accepts #RRGGBB uppercase") {
  CHECK(ParseHexColor("#E04300", 0) == 0xE04300u);
  CHECK(ParseHexColor("#000000", 0xDEADBEu) == 0x000000u);
  CHECK(ParseHexColor("#FFFFFF", 0) == 0xFFFFFFu);
}

TEST_CASE("ParseHexColor accepts lowercase and bare hex (no #)") {
  // The old firmware's /api/lights/color endpoint took both forms
  // depending on who wrote the query string; mirror that leniency.
  CHECK(ParseHexColor("e04300", 0) == 0xE04300u);
  CHECK(ParseHexColor("ff9900", 0) == 0xFF9900u);
}

TEST_CASE("ParseHexColor handles 3-digit shorthand by doubling each nibble") {
  // #rgb -> #rrggbb. Convention used by CSS; accept it so hand-written
  // URLs don't trip up. Explicit test so a future refactor can't
  // silently drop the shorthand path.
  CHECK(ParseHexColor("#f00", 0) == 0xFF0000u);
  CHECK(ParseHexColor("f00", 0) == 0xFF0000u);
  CHECK(ParseHexColor("abc", 0) == 0xAABBCCu);
}

TEST_CASE("ParseHexColor returns fallback on malformed input") {
  // Anything that isn't 3- or 6-digit hex must not parse. Callers
  // depend on this to detect invalid input instead of silently writing
  // a zero colour.
  CHECK(ParseHexColor("", 0x123456u) == 0x123456u);
  CHECK(ParseHexColor("zzzzzz", 0x123456u) == 0x123456u);
  CHECK(ParseHexColor("#12345", 0x123456u) == 0x123456u);    // too short
  CHECK(ParseHexColor("#1234567", 0x123456u) == 0x123456u);  // too long
  CHECK(ParseHexColor("#12345g", 0x123456u) == 0x123456u);   // non-hex
}

TEST_CASE("FormatHexColor writes uppercase 7-char string plus NUL") {
  char buf[8] = {};
  const size_t n = FormatHexColor(0xE04300u, buf);
  CHECK(n == 7);
  CHECK(std::string_view(buf) == "#E04300");

  FormatHexColor(0x000000u, buf);
  CHECK(std::string_view(buf) == "#000000");
  FormatHexColor(0xFFFFFFu, buf);
  CHECK(std::string_view(buf) == "#FFFFFF");
}

TEST_CASE("Parse → format → parse is idempotent for valid inputs") {
  // Canonical round-trip invariant — relied on by the status endpoint:
  // ParseHexColor decodes the WebUI's input, then FormatHexColor
  // re-emits it in the GET /api/lights response body. A mismatch
  // would make the UI's "current color" swatch drift.
  const char* kCases[] = {"#E04300", "#000000", "#FFFFFF", "#123456",
                          "#ABCDEF"};
  for (const char* s : kCases) {
    const uint32_t v = ParseHexColor(s, 0);
    char buf[8] = {};
    FormatHexColor(v, buf);
    CHECK(std::string_view(buf) == s);
    CHECK(ParseHexColor(buf, 0xDEADBEu) == v);
  }
}

TEST_CASE("FormatHexColor masks the top 8 bits before formatting") {
  // Callers sometimes pass a packed 0xRRGGBB with garbage in the top
  // byte (e.g. from a sign-extended parse). The formatter must only
  // emit the low 24 bits so the string stays 7 chars.
  char buf[8] = {};
  FormatHexColor(0xFF'E04300u, buf);
  CHECK(std::string_view(buf) == "#E04300");
}
