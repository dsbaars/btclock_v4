// Host tests for the pure-logic half of the HTTP Basic auth gate —
// base64 decode, "Basic " token extraction, user:pass split, and the
// constant-time compare. The IDF-side glue in auth_gate.cpp is not
// tested here (it touches httpd_req_t and NVS); this exercises every
// byte-level decision the gate makes on attacker-controlled input.

#include "doctest.h"

#include "auth_gate_logic.hpp"

#include <string>
#include <string_view>

using btclock::auth::ConstantTimeEquals;
using btclock::auth::CredentialsMatch;
using btclock::auth::DecodeBase64;
using btclock::auth::ExtractBasicToken;
using btclock::auth::ParseUserPass;

TEST_CASE("DecodeBase64 canonical user:pass") {
  // `echo -n "btclock:hunter2" | base64` -> "YnRjbG9jazpodW50ZXIy"
  std::string out;
  CHECK(DecodeBase64("YnRjbG9jazpodW50ZXIy", &out));
  CHECK(out == "btclock:hunter2");
}

TEST_CASE("DecodeBase64 handles single-byte '=' padding") {
  // "any carnal pleasure." -> "YW55IGNhcm5hbCBwbGVhc3VyZS4="
  std::string out;
  CHECK(DecodeBase64("YW55IGNhcm5hbCBwbGVhc3VyZS4=", &out));
  CHECK(out == "any carnal pleasure.");
}

TEST_CASE("DecodeBase64 handles double-byte '==' padding") {
  // "a:" -> "YTo="
  std::string out;
  CHECK(DecodeBase64("YTo=", &out));
  CHECK(out == "a:");

  // "foobar" -> "Zm9vYmFy"; "foob" -> "Zm9vYg==".
  CHECK(DecodeBase64("Zm9vYg==", &out));
  CHECK(out == "foob");
}

TEST_CASE("DecodeBase64 rejects non-multiple-of-4 length") {
  std::string out;
  CHECK_FALSE(DecodeBase64("abc", &out));
  CHECK(out.empty());
  CHECK_FALSE(DecodeBase64("abcde", &out));
}

TEST_CASE("DecodeBase64 rejects invalid characters") {
  std::string out;
  // '!' and ' ' aren't in the standard alphabet.
  CHECK_FALSE(DecodeBase64("ab!d", &out));
  CHECK_FALSE(DecodeBase64("ab cd", &out));
}

TEST_CASE("DecodeBase64 rejects padding in the middle") {
  std::string out;
  // `=` must only appear in the final quad.
  CHECK_FALSE(DecodeBase64("YWJ=YWJj", &out));
}

TEST_CASE("DecodeBase64 rejects lone trailing pad") {
  std::string out;
  // `Zm9=` would encode 1 byte but violates the 2-pad rule (the second
  // char has bits leaking into the would-be second byte).
  CHECK_FALSE(DecodeBase64("Z===", &out));
}

TEST_CASE("DecodeBase64 empty input decodes to empty output") {
  std::string out = "dirty";
  CHECK(DecodeBase64("", &out));
  CHECK(out.empty());
}

TEST_CASE("ParseUserPass splits on first colon only") {
  std::string u;
  std::string p;
  REQUIRE(ParseUserPass("btclock:hunter2", &u, &p));
  CHECK(u == "btclock");
  CHECK(p == "hunter2");

  // Password may contain further colons — HTTP Basic permits any byte
  // in the password slot.
  REQUIRE(ParseUserPass("user:pa:ss:word", &u, &p));
  CHECK(u == "user");
  CHECK(p == "pa:ss:word");
}

TEST_CASE("ParseUserPass allows empty username or password") {
  std::string u;
  std::string p;
  REQUIRE(ParseUserPass(":justpass", &u, &p));
  CHECK(u == "");
  CHECK(p == "justpass");

  REQUIRE(ParseUserPass("justuser:", &u, &p));
  CHECK(u == "justuser");
  CHECK(p == "");
}

TEST_CASE("ParseUserPass rejects missing colon") {
  std::string u = "old";
  std::string p = "old";
  CHECK_FALSE(ParseUserPass("noseparator", &u, &p));
  CHECK(u.empty());
  CHECK(p.empty());
}

TEST_CASE("ExtractBasicToken strips the scheme") {
  std::string_view tok;
  REQUIRE(ExtractBasicToken("Basic YnRjbG9jazpodW50ZXIy", &tok));
  CHECK(tok == "YnRjbG9jazpodW50ZXIy");
}

TEST_CASE("ExtractBasicToken is case-insensitive on the scheme name") {
  std::string_view tok;
  REQUIRE(ExtractBasicToken("BASIC YWJjZA==", &tok));
  CHECK(tok == "YWJjZA==");
  REQUIRE(ExtractBasicToken("basic YWJjZA==", &tok));
  CHECK(tok == "YWJjZA==");
  REQUIRE(ExtractBasicToken("BaSiC YWJjZA==", &tok));
  CHECK(tok == "YWJjZA==");
}

TEST_CASE("ExtractBasicToken tolerates multiple leading spaces") {
  std::string_view tok;
  REQUIRE(ExtractBasicToken("Basic    YWJjZA==", &tok));
  CHECK(tok == "YWJjZA==");
}

TEST_CASE("ExtractBasicToken rejects non-Basic schemes") {
  std::string_view tok;
  CHECK_FALSE(ExtractBasicToken("Bearer sometoken", &tok));
  CHECK_FALSE(ExtractBasicToken("Digest user=foo", &tok));
}

TEST_CASE("ExtractBasicToken rejects missing token") {
  std::string_view tok;
  CHECK_FALSE(ExtractBasicToken("Basic ", &tok));
  CHECK_FALSE(ExtractBasicToken("Basic", &tok));
}

TEST_CASE("ConstantTimeEquals basic equality") {
  CHECK(ConstantTimeEquals("hunter2", "hunter2"));
  CHECK_FALSE(ConstantTimeEquals("hunter2", "hunter3"));
}

TEST_CASE("ConstantTimeEquals rejects length mismatch without shortcut") {
  // The function walks max(len_a, len_b) — the test just asserts the
  // return value; the timing property isn't directly observable here
  // but the implementation contract is documented in the header.
  CHECK_FALSE(ConstantTimeEquals("short", "shorterish"));
  CHECK_FALSE(ConstantTimeEquals("shorterish", "short"));
}

TEST_CASE("ConstantTimeEquals empty strings compare equal") {
  CHECK(ConstantTimeEquals("", ""));
  CHECK_FALSE(ConstantTimeEquals("", "x"));
  CHECK_FALSE(ConstantTimeEquals("x", ""));
}

TEST_CASE("CredentialsMatch happy path") {
  CHECK(CredentialsMatch("btclock", "hunter2", "btclock", "hunter2"));
}

TEST_CASE("CredentialsMatch mismatched user") {
  CHECK_FALSE(CredentialsMatch("mallory", "hunter2", "btclock", "hunter2"));
}

TEST_CASE("CredentialsMatch mismatched pass") {
  CHECK_FALSE(CredentialsMatch("btclock", "wrong", "btclock", "hunter2"));
}

TEST_CASE("CredentialsMatch both wrong") {
  // The function evaluates both halves regardless of the first result,
  // but the API contract is "false unless both match". Confirm that.
  CHECK_FALSE(CredentialsMatch("", "", "btclock", "hunter2"));
}

TEST_CASE("End-to-end: header bytes -> credential match") {
  // Exact path the gate walks for a successful request.
  std::string_view header = "Basic YnRjbG9jazpodW50ZXIy";
  std::string_view tok;
  REQUIRE(ExtractBasicToken(header, &tok));

  std::string decoded;
  REQUIRE(DecodeBase64(tok, &decoded));

  std::string u;
  std::string p;
  REQUIRE(ParseUserPass(decoded, &u, &p));

  CHECK(CredentialsMatch(u, p, "btclock", "hunter2"));
}

TEST_CASE("End-to-end rejects password stuffed into username slot") {
  // "btclockhunter2:" splits with empty password — configured pass is
  // non-empty so this must fail even though a naive concatenation check
  // would have accepted it.
  std::string_view header = "Basic YnRjbG9ja2h1bnRlcjI6";  // "btclockhunter2:"
  std::string_view tok;
  REQUIRE(ExtractBasicToken(header, &tok));
  std::string decoded;
  REQUIRE(DecodeBase64(tok, &decoded));
  std::string u;
  std::string p;
  REQUIRE(ParseUserPass(decoded, &u, &p));
  CHECK_FALSE(CredentialsMatch(u, p, "btclock", "hunter2"));
}

TEST_CASE("End-to-end rejects swapped user/pass") {
  // "hunter2:btclock" base64 — must not authenticate as "btclock/hunter2".
  std::string_view header = "Basic aHVudGVyMjpidGNsb2Nr";
  std::string_view tok;
  REQUIRE(ExtractBasicToken(header, &tok));
  std::string decoded;
  REQUIRE(DecodeBase64(tok, &decoded));
  std::string u;
  std::string p;
  REQUIRE(ParseUserPass(decoded, &u, &p));
  CHECK_FALSE(CredentialsMatch(u, p, "btclock", "hunter2"));
}

TEST_CASE("CredentialsMatch with empty configured_pass accepts nothing") {
  // The IDF glue in auth_gate.cpp lets requests through when
  // httpAuthPass is empty (documented recovery path against
  // lockout); the pure-logic comparator does NOT make that exception
  // — an attacker-supplied empty password must still fail when paired
  // with a real username. The exception lives only in the glue.
  CHECK_FALSE(CredentialsMatch("btclock", "", "btclock", "hunter2"));
}

TEST_CASE("CredentialsMatch treats UTF-8 bytes as opaque") {
  // Non-ASCII password bytes round-trip through base64 and must compare
  // byte-for-byte — the constant-time path does not normalise or casefold.
  const char kPw[] = "p\xC3\xA4ssw\xC3\xB6rd";  // "pässwörd"
  CHECK(CredentialsMatch("btclock", kPw, "btclock", kPw));
  CHECK_FALSE(CredentialsMatch("btclock", "passwoerd", "btclock", kPw));
}

TEST_CASE("ExtractBasicToken rejects tab-separated scheme") {
  // RFC 7235 §2.1 allows SP between scheme and credentials; strict
  // readers reject HTAB. The parser deliberately rejects anything
  // other than ASCII SP to avoid surprise-acceptance of smuggled
  // headers.
  std::string_view tok;
  CHECK_FALSE(ExtractBasicToken("Basic\tYnRjbG9jazpodW50ZXIy", &tok));
}

TEST_CASE("DecodeBase64 rejects url-safe alphabet") {
  // Some clients emit url-safe base64 with `-` / `_` in place of `+` / `/`.
  // The Authorization header uses the standard alphabet; urlsafe input is
  // ambiguous and we don't want to silently accept a double spelling.
  std::string out;
  CHECK_FALSE(DecodeBase64("YWJj-_==", &out));
}
