#include "mime.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace btclock {
namespace {

// Case-insensitive suffix compare: does `path` end with `ext`?
// `ext` must already be lowercase; `path` can be any case.
bool EndsWithCi(std::string_view path, std::string_view ext) {
  if (path.size() < ext.size()) return false;
  const size_t off = path.size() - ext.size();
  for (size_t i = 0; i < ext.size(); ++i) {
    const char c = path[off + i];
    const char lc = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    if (lc != ext[i]) return false;
  }
  return true;
}

}  // namespace

std::string_view MimeTypeForPath(std::string_view path) {
  // A trailing .gz is transparent — the file is compressed in transit
  // (served with Content-Encoding: gzip), but its Content-Type is still
  // determined by the original extension. Strip one trailing .gz so
  // `foo.css.gz` dispatches to .css below.
  if (EndsWithCi(path, ".gz")) {
    path.remove_suffix(3);
  }

  // Ordered by observed frequency in the WebUI bundle (Vite output):
  // .js and .css dominate, then .html for the entry, then fonts, then
  // small images. Short-circuits the common case.
  if (EndsWithCi(path, ".html") || EndsWithCi(path, ".htm")) {
    return "text/html";
  }
  if (EndsWithCi(path, ".js") || EndsWithCi(path, ".mjs")) {
    return "application/javascript";
  }
  if (EndsWithCi(path, ".css")) return "text/css";
  if (EndsWithCi(path, ".json")) return "application/json";
  if (EndsWithCi(path, ".svg")) return "image/svg+xml";
  if (EndsWithCi(path, ".png")) return "image/png";
  if (EndsWithCi(path, ".jpg") || EndsWithCi(path, ".jpeg")) {
    return "image/jpeg";
  }
  if (EndsWithCi(path, ".gif")) return "image/gif";
  if (EndsWithCi(path, ".webp")) return "image/webp";
  if (EndsWithCi(path, ".ico")) return "image/x-icon";
  if (EndsWithCi(path, ".woff2")) return "font/woff2";
  if (EndsWithCi(path, ".woff")) return "font/woff";
  if (EndsWithCi(path, ".ttf")) return "font/ttf";
  if (EndsWithCi(path, ".xml")) return "application/xml";
  if (EndsWithCi(path, ".txt")) return "text/plain";
  if (EndsWithCi(path, ".map")) return "application/json";  // source maps
  // Fallback matches AsyncWebServer's contentType(): if we don't know,
  // hand the browser octet-stream and let it decide what to do.
  return "application/octet-stream";
}

}  // namespace btclock
