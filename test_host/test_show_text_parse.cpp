// Host tests for the /api/show/text and /api/show/custom JSON parsers.
// Pins the old-firmware parity rules (one-char-per-panel for text,
// array-of-strings verbatim for custom) against the bare-cJSON
// implementation in components/webserver/show_text_parse.cpp.

#include <string>

#include "doctest.h"
#include "show_text_parse.hpp"

using btclock::ParseShowCustomBody;
using btclock::ParseShowTextBody;

TEST_CASE("ParseShowTextBody: missing body yields empty cells on 7 panels") {
  // An empty JSON body is a "clear the custom screen" request — the
  // handler uses the result to push all-empty cells to the renderer.
  auto r = ParseShowTextBody("", 7);
  CHECK(r.ok);
  REQUIRE(r.cells.size() == 7);
  for (const auto& c : r.cells) CHECK(c.empty());
}

TEST_CASE("ParseShowTextBody: places one character per panel, uppercased") {
  auto r = ParseShowTextBody(R"({"text":"hello"})", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "H");
  CHECK(r.cells[1] == "E");
  CHECK(r.cells[2] == "L");
  CHECK(r.cells[3] == "L");
  CHECK(r.cells[4] == "O");
  CHECK(r.cells[5].empty());
  CHECK(r.cells[6].empty());
}

TEST_CASE("ParseShowTextBody: oversized text truncates at panel count") {
  // Old firmware clamps tLen to NUM_SCREENS and silently drops the rest
  // (src/lib/net/webserver/actions.cpp:80). Match that — no error, just
  // a dropped tail.
  auto r = ParseShowTextBody(R"({"text":"ABCDEFGHIJ"})", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "A");
  CHECK(r.cells[6] == "G");  // H, I, J dropped silently
}

TEST_CASE("ParseShowTextBody: numeric digits pass through") {
  // The old /api/show/number/{n} rewrite routes digits straight to
  // /api/show/text — verify a numeric payload produces per-digit cells.
  auto r = ParseShowTextBody(R"({"text":"12345"})", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "1");
  CHECK(r.cells[4] == "5");
  CHECK(r.cells[5].empty());
}

TEST_CASE("ParseShowTextBody: invalid JSON returns bad_json") {
  auto r = ParseShowTextBody("{not json", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowTextBody: missing text field returns missing_text") {
  auto r = ParseShowTextBody(R"({"other":"value"})", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "missing_text");
}

TEST_CASE("ParseShowTextBody: non-string text field returns missing_text") {
  auto r = ParseShowTextBody(R"({"text":42})", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "missing_text");
}

TEST_CASE("ParseShowTextBody: root array is rejected") {
  // Array-shaped body is the /api/show/custom format, not /api/show/text.
  // Returning bad_json keeps clients from accidentally silently-routing
  // to the wrong endpoint.
  auto r = ParseShowTextBody(R"(["A","B"])", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowTextBody: zero panels is a parse failure") {
  auto r = ParseShowTextBody(R"({"text":"A"})", 0);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "no_panels");
}

TEST_CASE("ParseShowCustomBody: bare array (old firmware wire format)") {
  auto r = ParseShowCustomBody(R"(["BLOCK/HEIGHT","8","3","1"])", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "BLOCK/HEIGHT");
  CHECK(r.cells[1] == "8");
  CHECK(r.cells[2] == "3");
  CHECK(r.cells[3] == "1");
  CHECK(r.cells[4].empty());
  CHECK(r.cells[5].empty());
  CHECK(r.cells[6].empty());
}

TEST_CASE("ParseShowCustomBody: wrapper object with cells key") {
  auto r = ParseShowCustomBody(R"({"cells":["A","B","C"]})", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "A");
  CHECK(r.cells[1] == "B");
  CHECK(r.cells[2] == "C");
  CHECK(r.cells[3].empty());
}

TEST_CASE("ParseShowCustomBody: string array trimmed to panel count") {
  auto r = ParseShowCustomBody(R"(["A","B","C","D","E","F","G","H","I"])", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  // Cell 7 and 8 silently dropped, matching the old firmware's
  // `if (i >= NUM_SCREENS) break;` in onApiShowTextAdvanced.
  CHECK(r.cells[6] == "G");
}

TEST_CASE("ParseShowCustomBody: shorter array zero-pads trailing cells") {
  auto r = ParseShowCustomBody(R"(["ONE","TWO"])", 7);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 7);
  CHECK(r.cells[0] == "ONE");
  CHECK(r.cells[1] == "TWO");
  for (std::size_t i = 2; i < r.cells.size(); ++i) CHECK(r.cells[i].empty());
}

TEST_CASE("ParseShowCustomBody: non-string element rejected") {
  // Old firmware relied on ArduinoJson's as<String>() which silently
  // coerced — we're stricter here so the caller learns about bad data
  // instead of rendering a surprising "null" or "42" cell.
  auto r = ParseShowCustomBody(R"(["A",42,"C"])", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowCustomBody: invalid JSON returns bad_json") {
  auto r = ParseShowCustomBody("not json at all", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowCustomBody: empty body returns bad_json") {
  auto r = ParseShowCustomBody("", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowCustomBody: wrong-shape object returns bad_json") {
  auto r = ParseShowCustomBody(R"({"items":["A","B"]})", 7);
  CHECK_FALSE(r.ok);
  CHECK(r.error == "bad_json");
}

TEST_CASE("ParseShowCustomBody: 8-panel V8 device gets all 8 cells") {
  auto r = ParseShowCustomBody(R"(["A","B","C","D","E","F","G","H"])", 8);
  REQUIRE(r.ok);
  REQUIRE(r.cells.size() == 8);
  CHECK(r.cells[7] == "H");
}
