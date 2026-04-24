// Pure-logic helpers for the HTTP Basic auth gate. Split out from
// auth_gate.hpp so the parsing + compare paths can be covered by the
// host test suite without dragging in esp_http_server / NVS.
//
// The functions here don't talk to the wire; the IDF-side glue in
// auth_gate.cpp reads the `Authorization` header and calls them. Each
// helper is deliberately tiny so the attack surface on a request that
// an attacker controls is well understood:
//   * `DecodeBase64` does standard base64 (no url-safe alphabet).
//   * `ParseBasicCredential` splits on the first `:` only; passwords
//     may legitimately contain colons.
//   * `ConstantTimeEquals` avoids early-exit timing leaks on both
//     username and password compares.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace btclock {
namespace auth {

// Decode a standard-alphabet base64 string. Whitespace inside `src` is
// rejected (the Authorization header is a single token). On success the
// decoded bytes are appended to `*out` and the function returns true;
// on failure `*out` is cleared and returns false.
bool DecodeBase64(std::string_view src, std::string* out);

// Given a *decoded* "user:pass" buffer, split on the first colon.
// Returns false if no colon is present (malformed credential).
// Password may contain any bytes including further colons — only the
// first delimits the split. Empty user or empty pass is allowed here;
// the caller decides whether that's acceptable.
bool ParseUserPass(std::string_view decoded, std::string* user,
                   std::string* pass);

// Strip the literal `Basic ` prefix from an Authorization header value
// (case-insensitive on the scheme name, per RFC 7235 §2.1). Returns
// false if the header isn't a Basic credential. On success `*out`
// points inside `value` — caller must keep `value` alive.
bool ExtractBasicToken(std::string_view value, std::string_view* out);

// Constant-time equality for two byte strings. Returns true iff the
// lengths match and every byte is equal. Crucially, the comparison
// walks the longer of the two lengths with zero-padding so a length
// mismatch doesn't short-circuit and leak the real secret length.
bool ConstantTimeEquals(std::string_view a, std::string_view b);

// Combined check: given the decoded header credential (user+pass) and
// the configured credential, return true iff both halves match in
// constant time. Exposed so the host test can cover the compose.
bool CredentialsMatch(std::string_view supplied_user,
                      std::string_view supplied_pass,
                      std::string_view configured_user,
                      std::string_view configured_pass);

}  // namespace auth
}  // namespace btclock
