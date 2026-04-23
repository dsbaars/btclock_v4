// Extension → Content-Type inference for the static file handler.
//
// Pure-logic helper — no ESP-IDF dependency — so host tests can exercise
// the full mapping table. The static file server (control_server.cpp)
// uses this to set the response Content-Type when serving assets out of
// /lfs/www/.
//
// Behaviour:
//   * Lookup is case-insensitive on the extension.
//   * A `.gz` suffix is transparent: `foo.js.gz` maps to the Content-Type
//     for `.js` — the `Content-Encoding: gzip` header carries the
//     "gzipped" information, not Content-Type.
//   * An unknown or missing extension returns `application/octet-stream`
//     — matches the behaviour of AsyncWebServer's contentType() fallback
//     (which is what the old Arduino firmware used via `serveStatic`).

#pragma once

#include <string_view>

namespace btclock {

// Returns the Content-Type string for the given URL path, based on the
// file extension. The returned view is a string literal with static
// lifetime; safe to stash on the stack or pass to httpd_resp_set_type.
//
// `path` may be the full URL path or just a filename. Query strings
// ("?foo=bar") should be stripped before calling — this function does
// NOT handle them.
std::string_view MimeTypeForPath(std::string_view path);

}  // namespace btclock
