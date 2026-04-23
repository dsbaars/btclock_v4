// Host tests for the MIME-type inference helper used by the static
// WebUI handler in components/webserver. Pure logic — no ESP-IDF.

#include "doctest.h"

#include "mime.hpp"

using btclock::MimeTypeForPath;

TEST_CASE("MimeTypeForPath maps well-known WebUI extensions") {
  CHECK(MimeTypeForPath("/index.html") == "text/html");
  CHECK(MimeTypeForPath("/foo.htm") == "text/html");
  CHECK(MimeTypeForPath("/assets/app.js") == "application/javascript");
  CHECK(MimeTypeForPath("/assets/app.mjs") == "application/javascript");
  CHECK(MimeTypeForPath("/assets/app.css") == "text/css");
  CHECK(MimeTypeForPath("/api/settings.json") == "application/json");
  CHECK(MimeTypeForPath("/logo.svg") == "image/svg+xml");
  CHECK(MimeTypeForPath("/banner.png") == "image/png");
  CHECK(MimeTypeForPath("/photo.jpg") == "image/jpeg");
  CHECK(MimeTypeForPath("/photo.jpeg") == "image/jpeg");
  CHECK(MimeTypeForPath("/tile.webp") == "image/webp");
  CHECK(MimeTypeForPath("/favicon.ico") == "image/x-icon");
  CHECK(MimeTypeForPath("/fonts/inter.woff2") == "font/woff2");
  CHECK(MimeTypeForPath("/fonts/inter.woff") == "font/woff");
  CHECK(MimeTypeForPath("/fonts/inter.ttf") == "font/ttf");
  CHECK(MimeTypeForPath("/sitemap.xml") == "application/xml");
  CHECK(MimeTypeForPath("/robots.txt") == "text/plain");
}

TEST_CASE("MimeTypeForPath treats trailing .gz as transparent") {
  // The static handler emits Content-Encoding: gzip for these; the
  // Content-Type must reflect the *inner* file type so the browser
  // decodes and parses correctly.
  CHECK(MimeTypeForPath("/index.html.gz") == "text/html");
  CHECK(MimeTypeForPath("/assets/app.js.gz") == "application/javascript");
  CHECK(MimeTypeForPath("/assets/app.css.gz") == "text/css");
  CHECK(MimeTypeForPath("/logo.svg.gz") == "image/svg+xml");
  CHECK(MimeTypeForPath("/fonts/inter.woff2.gz") == "font/woff2");
}

TEST_CASE("MimeTypeForPath is case-insensitive on extension") {
  CHECK(MimeTypeForPath("/Index.HTML") == "text/html");
  CHECK(MimeTypeForPath("/App.JS") == "application/javascript");
  CHECK(MimeTypeForPath("/logo.SVG") == "image/svg+xml");
  // Case-insensitive on the .gz suffix too.
  CHECK(MimeTypeForPath("/app.js.GZ") == "application/javascript");
}

TEST_CASE("MimeTypeForPath falls back to octet-stream for unknown/empty") {
  CHECK(MimeTypeForPath("/foo.unknown") == "application/octet-stream");
  CHECK(MimeTypeForPath("/noext") == "application/octet-stream");
  CHECK(MimeTypeForPath("") == "application/octet-stream");
  // A bare ".gz" with nothing before it — after stripping we get an
  // empty path which falls through to the default.
  CHECK(MimeTypeForPath(".gz") == "application/octet-stream");
}

TEST_CASE("MimeTypeForPath works on filename-only inputs") {
  // Some call sites may pass just the basename; the mapping must still
  // work identically because it's suffix-matching.
  CHECK(MimeTypeForPath("index.html") == "text/html");
  CHECK(MimeTypeForPath("app.js.gz") == "application/javascript");
  CHECK(MimeTypeForPath("font.woff2") == "font/woff2");
}

TEST_CASE("MimeTypeForPath handles source-map extension") {
  // Vite emits .map sourcemaps alongside .js bundles; the browser
  // consumes them as JSON.
  CHECK(MimeTypeForPath("/assets/app.js.map") == "application/json");
}
