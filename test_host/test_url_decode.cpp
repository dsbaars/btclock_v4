// Host tests for the in-place query-string decoder used by every
// /api/show/* endpoint. The user-visible bug that motivated the helper
// was `?t=%20CLOCK%20` rendering literally as `%20CLOCK%20`; the first
// test below pins that exact case so it can never regress silently.

#include <cstring>
#include <string>

#include "doctest.h"
#include "url_decode.hpp"

using btclock::http::UrlDecodeInPlace;

namespace {

// `UrlDecodeInPlace` is allocation-free and writes through a `char*`,
// so the tests need a writable buffer per case. Returning a string
// keeps the assertions readable.
std::string Decode(const char* in, bool* ok_out) {
  char buf[256];
  std::strncpy(buf, in, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  *ok_out = UrlDecodeInPlace(buf);
  return std::string(buf);
}

}  // namespace

TEST_CASE("UrlDecodeInPlace: plain ASCII passes through") {
  bool ok = false;
  auto s = Decode("CLOCK", &ok);
  CHECK(ok);
  CHECK(s == "CLOCK");
}

TEST_CASE("UrlDecodeInPlace: %20 mid-string decodes to space") {
  bool ok = false;
  auto s = Decode("HELLO%20WORLD", &ok);
  CHECK(ok);
  CHECK(s == "HELLO WORLD");
}

TEST_CASE("UrlDecodeInPlace: leading and trailing %20 (the user bug)") {
  // /api/show/text?t=%20CLOCK%20 was rendering `%20CLOCK%20` literally.
  bool ok = false;
  auto s = Decode("%20CLOCK%20", &ok);
  CHECK(ok);
  CHECK(s == " CLOCK ");
}

TEST_CASE("UrlDecodeInPlace: + decodes to space") {
  // Browsers and curl form-encode space as `+` even in query strings;
  // the old btclock_v3 firmware decoded it that way too.
  bool ok = false;
  auto s = Decode("HELLO+WORLD", &ok);
  CHECK(ok);
  CHECK(s == "HELLO WORLD");
}

TEST_CASE("UrlDecodeInPlace: + and %XX mix, multi-byte UTF-8 round-trips") {
  bool ok = false;
  auto s = Decode("HI+%E2%9A%A1", &ok);
  CHECK(ok);
  CHECK(s == "HI \xE2\x9A\xA1");  // U+26A1 HIGH VOLTAGE SIGN
}

TEST_CASE("UrlDecodeInPlace: lowercase hex digits accepted") {
  bool ok = false;
  auto s = Decode("%e2%9a%a1", &ok);
  CHECK(ok);
  CHECK(s == "\xE2\x9A\xA1");
}

TEST_CASE("UrlDecodeInPlace: empty string stays empty") {
  bool ok = false;
  auto s = Decode("", &ok);
  CHECK(ok);
  CHECK(s.empty());
}

TEST_CASE("UrlDecodeInPlace: truncated %X rejected") {
  // Only one hex digit after `%` — the second byte is the NUL
  // terminator, so the decoder must refuse to read past it.
  bool ok = true;
  Decode("a%2", &ok);
  CHECK_FALSE(ok);
}

TEST_CASE("UrlDecodeInPlace: non-hex digit in escape rejected") {
  bool ok = true;
  Decode("a%2g", &ok);
  CHECK_FALSE(ok);
}

TEST_CASE("UrlDecodeInPlace: lone % at end of string rejected") {
  bool ok = true;
  Decode("a%", &ok);
  CHECK_FALSE(ok);
}
