// Application-side font bundle.
//
// Loads every TTF the firmware ships and exposes them both by name
// (antonio / oswald / dejavu) and via a selectable FontFamily enum
// that the production firmware's `fontName` preference maps to.

#pragma once

#include <array>
#include <cstdint>

#include "font.hpp"

namespace btclock {

// 16 sats-symbol variants live in the Satoshi Symbol font at codepoints
// U+E000..U+E00F. SatsGlyphUtf8(variant) returns a 4-byte buffer
// containing the UTF-8 encoding of U+E000+variant, null-terminated —
// pass buf.data() to the render helpers.
//
// The "production default" variant is U+E007. The NVS-backed user
// preference lives at ("ui", "sats_variant") and goes through
// ClampSatsVariant() on read so an out-of-range stored value falls back
// to the default rather than silently wrapping via the bitmask in
// SatsGlyphUtf8.
inline constexpr uint8_t kSatsVariantDefault = 7;
inline constexpr uint8_t kSatsVariantMax = 15;

// A stored value of e.g. 100 would mask to 4 in SatsGlyphUtf8, which
// is a glyph the user never picked — prefer the documented default.
inline constexpr uint8_t ClampSatsVariant(uint32_t raw) {
  return raw <= kSatsVariantMax ? static_cast<uint8_t>(raw)
                                : kSatsVariantDefault;
}

struct SatsGlyphUtf8Buf {
  std::array<char, 5> bytes{};
  const char* c_str() const { return bytes.data(); }
};

inline SatsGlyphUtf8Buf SatsGlyphUtf8(uint8_t variant) {
  SatsGlyphUtf8Buf out;
  const uint32_t cp = 0xE000u + (variant & 0x0Fu);
  // All 16 variants are in the 3-byte UTF-8 range (U+0800..U+FFFF).
  out.bytes[0] = static_cast<char>(0xE0u | (cp >> 12));
  out.bytes[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
  out.bytes[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
  out.bytes[3] = '\0';
  return out;
}

enum class FontFamily : uint8_t {
  kAntonio = 0,
  kOswald = 1,
  kDejaVu = 2,
};

struct FontBundle {
  const Font* regular;
  const Font* bold;
};

class AppFonts {
 public:
  AppFonts();

  const Font& antonio() const { return antonio_; }
  const Font& oswald() const { return oswald_; }
  const Font& oswald_bold() const { return oswald_bold_; }
  const Font& dejavu() const { return dejavu_; }
  const Font& dejavu_bold() const { return dejavu_bold_; }
  // Subsetted to just the sats prefix glyph — only call for the literal
  // letter "S" when you want the sats marker rendered.
  const Font& sats_symbol() const { return sats_symbol_; }
  // Material Design Icons subsetted to the few glyphs the firmware
  // paints (lightning-bolt, pickaxe, rocket-launch at the time of
  // writing). Pass the codepoints from mdi_codepoints.hpp. See
  // tools/fonts/regen_mdi.sh to add more.
  const Font& mdi() const { return mdi_; }

  // Resolve a FontFamily into its (regular, bold) pair. Antonio has no
  // separate bold variant — its regular is used for both roles; callers
  // that specifically need bold markup should use Oswald or DejaVu.
  FontBundle Bundle(FontFamily f) const;

 private:
  Font antonio_;
  Font oswald_;
  Font oswald_bold_;
  Font dejavu_;
  Font dejavu_bold_;
  Font sats_symbol_;
  Font mdi_;
};

}  // namespace btclock
