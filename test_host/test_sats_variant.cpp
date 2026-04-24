#include "doctest.h"

#include <cstdint>
#include <cstring>

#include "fonts_app.hpp"

// ClampSatsVariant is the NVS read-path guard — if the stored value is
// out of range, falling back to the documented default keeps the Moscow
// screen showing a glyph the user actually picked rather than an alias
// produced by SatsGlyphUtf8's internal bitmask (`variant & 0x0F`).

TEST_CASE("ClampSatsVariant passes in-range values straight through") {
  for (uint32_t v = 0; v <= btclock::kSatsVariantMax; ++v) {
    CHECK(btclock::ClampSatsVariant(v) == v);
  }
}

TEST_CASE("ClampSatsVariant returns the documented default at 16 and above") {
  CHECK(btclock::ClampSatsVariant(16) == btclock::kSatsVariantDefault);
  CHECK(btclock::ClampSatsVariant(255) == btclock::kSatsVariantDefault);
  // NVS stores the value as u32 — guard against a misencoded write.
  CHECK(btclock::ClampSatsVariant(0xFFFFFFFFu) == btclock::kSatsVariantDefault);
}

TEST_CASE("SatsGlyphUtf8 encodes each variant as the correct 3-byte codepoint") {
  // U+E000+variant lives in the UTF-8 3-byte range — [0xEE, 0x80|..., 0x80|...].
  for (uint8_t v = 0; v <= btclock::kSatsVariantMax; ++v) {
    const auto buf = btclock::SatsGlyphUtf8(v);
    const uint32_t cp = 0xE000u + v;
    const unsigned char b0 = static_cast<unsigned char>(buf.bytes[0]);
    const unsigned char b1 = static_cast<unsigned char>(buf.bytes[1]);
    const unsigned char b2 = static_cast<unsigned char>(buf.bytes[2]);
    CHECK(b0 == (0xE0u | (cp >> 12)));
    CHECK(b1 == (0x80u | ((cp >> 6) & 0x3Fu)));
    CHECK(b2 == (0x80u | (cp & 0x3Fu)));
    CHECK(buf.bytes[3] == '\0');
  }
}

TEST_CASE("SatsGlyphUtf8 default matches variant 7") {
  const auto def = btclock::SatsGlyphUtf8(btclock::kSatsVariantDefault);
  const auto v7 = btclock::SatsGlyphUtf8(7);
  CHECK(std::strcmp(def.c_str(), v7.c_str()) == 0);
}
