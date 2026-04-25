// In-place RFC 3986 / x-www-form-urlencoded decoder for query-string
// values. ESP-IDF's `httpd_query_key_value()` does NOT percent-decode
// the value it returns; without this every endpoint that takes a
// human-typed string (e.g. `/api/show/text?t=%20CLOCK%20`) would render
// the literal `%20`s. Decoding in-place keeps the helper
// allocation-free and lets callers hand the same fixed-size buffer
// they already pass to `httpd_query_key_value()`.
//
// Behaviour:
//   `%XX`  -> byte with hex value XX (case-insensitive). Malformed
//             escapes (truncated, non-hex digit, `%` at end of string)
//             cause the function to return false; the caller should
//             reject the request with 400.
//   `+`    -> space. Browsers and curl form-encode space as `+` even in
//             query strings, and the old btclock_v3 firmware (Arduino's
//             ESPAsyncWebServer) decoded it that way too — match.
//   else   -> passes through.
//
// Output is always shorter than or equal to input, so decoding into the
// same buffer is safe.
//
// The function is split into its own translation unit so the host test
// suite can exercise it without dragging in `esp_http_server`.
#pragma once

namespace btclock {
namespace http {

// Decode `buf` in place. Returns true on success, false if `buf`
// contains a malformed `%`-escape (in which case `buf` may be left
// partially decoded and must be discarded).
//
// `buf` MUST be NUL-terminated. The result is also NUL-terminated.
bool UrlDecodeInPlace(char* buf);

}  // namespace http
}  // namespace btclock
