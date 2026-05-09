// libFuzzer harness for components/webserver/url_decode.cpp.
//
// UrlDecodeInPlace is reached on every HTTP request that takes a
// query-string value (legacy fallbacks like `/api/show/text?t=...`,
// `/api/show/currency?c=...`, and every PATCH that round-trips through
// `httpd_query_key_value`). It's a
// hand-rolled byte state machine with `%XX` hex decoding, `+`-to-space
// rules, and an "in-place" write contract — three categories that have
// historically been a CVE-magnet for HTTP servers.
//
// The function expects a NUL-terminated buffer it can write through, so
// the harness materialises one. ASan + UBSan catch the practical bug
// classes: OOB read past the NUL on truncated `%X` escapes, OOB write
// past the buffer on (currently impossible but theoretically writeable)
// expansion paths, signed overflow on hex-digit composition.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "url_decode.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  // Copy into a writable, NUL-terminated buffer. UrlDecodeInPlace's
  // contract requires NUL-termination — feeding raw bytes from libFuzzer
  // would let the decoder run past the input. Embedded NULs in `data`
  // are passed through verbatim because the function stops at the
  // first NUL it encounters; that's part of what we want to fuzz (a
  // caller passing a buffer with a stray NUL mid-string should not
  // cause the decoder to misbehave on whatever follows).
  std::vector<char> buf(size + 1);
  if (size > 0) std::memcpy(buf.data(), data, size);
  buf[size] = '\0';

  (void)btclock::http::UrlDecodeInPlace(buf.data());
  return 0;
}
