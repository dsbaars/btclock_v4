// Pin the mini-markdown parse rules font.cpp's DrawMarkdown depends on.
// The rules are tiny but the trim-trailing-whitespace branch + the
// "any '*' in line" stripping are easy to break in a copy-paste edit.

#include <string>
#include <vector>

#include "doctest.h"
#include "markdown_parse.hpp"

using btclock::MarkdownLine;
using btclock::ParseMarkdownLines;

TEST_CASE("ParseMarkdownLines empty input yields one empty line") {
  // The flush-at-EOF path means "" still produces one (empty,
  // non-bold) line. DrawMarkdown skips empty lines visually but the
  // line count drives vertical centring, so the parser never silently
  // drops the trailing "line."
  const auto lines = ParseMarkdownLines("");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "");
  CHECK_FALSE(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines null input yields no lines") {
  const auto lines = ParseMarkdownLines(nullptr);
  CHECK(lines.empty());
}

TEST_CASE("ParseMarkdownLines single regular line") {
  const auto lines = ParseMarkdownLines("hello world");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "hello world");
  CHECK_FALSE(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines bold line strips all asterisks") {
  const auto lines = ParseMarkdownLines("*Hostname*");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "Hostname");
  CHECK(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines bold line trims trailing whitespace") {
  // "*SSID: *" parses bold, '*' strip leaves "SSID: ", trailing space
  // trimmed to "SSID:". Provisioning UI relies on this so the colon
  // sits flush against the next line's value.
  const auto lines = ParseMarkdownLines("*SSID: *");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "SSID:");
  CHECK(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines bold line trims trailing tabs too") {
  const auto lines = ParseMarkdownLines("*Label*\t");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "Label");
  CHECK(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines newline splits lines") {
  const auto lines = ParseMarkdownLines("first\nsecond");
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].text == "first");
  CHECK_FALSE(lines[0].is_bold);
  CHECK(lines[1].text == "second");
  CHECK_FALSE(lines[1].is_bold);
}

TEST_CASE("ParseMarkdownLines mixes bold and regular lines") {
  const auto lines = ParseMarkdownLines("*Title*\nbody text\n*Note*\nmore");
  REQUIRE(lines.size() == 4);
  CHECK(lines[0].text == "Title");
  CHECK(lines[0].is_bold);
  CHECK(lines[1].text == "body text");
  CHECK_FALSE(lines[1].is_bold);
  CHECK(lines[2].text == "Note");
  CHECK(lines[2].is_bold);
  CHECK(lines[3].text == "more");
  CHECK_FALSE(lines[3].is_bold);
}

TEST_CASE("ParseMarkdownLines drops carriage returns") {
  // CRLF-formatted input from a web form shouldn't produce extra
  // blank lines — the firmware always treats '\n' alone as the line
  // separator.
  const auto lines = ParseMarkdownLines("first\r\nsecond\r\n");
  REQUIRE(lines.size() == 3);
  CHECK(lines[0].text == "first");
  CHECK(lines[1].text == "second");
  CHECK(lines[2].text == "");
}

TEST_CASE("ParseMarkdownLines bold marker only on first character") {
  // 'a*b' is not bold — only a leading '*' marks bold. The historical
  // firmware behaves the same way.
  const auto lines = ParseMarkdownLines("a*b");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "a*b");
  CHECK_FALSE(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines bold line with embedded asterisk") {
  // All '*' in a bold line are stripped, not just the marker pair.
  const auto lines = ParseMarkdownLines("*foo*bar*");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "foobar");
  CHECK(lines[0].is_bold);
}

TEST_CASE("ParseMarkdownLines empty bold line collapses to empty regular") {
  // "**" is bold-marked but contains no payload. After strip + trim,
  // text is empty — the bold flag is still true (matches the original
  // implementation; DrawMarkdown skips the row visually anyway).
  const auto lines = ParseMarkdownLines("**");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text == "");
  CHECK(lines[0].is_bold);
}

TEST_CASE(
    "ParseMarkdownLines trailing newline produces a trailing empty line") {
  // Same flush-at-EOF reason as the empty-input case — the count
  // matters for vertical centring.
  const auto lines = ParseMarkdownLines("foo\n");
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].text == "foo");
  CHECK(lines[1].text == "");
}
