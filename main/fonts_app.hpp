// Application-side font bundle.
//
// Loads every TTF the firmware ships and exposes them both by name
// (antonio / oswald / inter / sourceSerif / merriweather / bitter /
// atkinson) and via a selectable FontFamily enum that the production
// firmware's `fontName` preference maps to.

#pragma once

#include <array>
#include <cstdint>
#include <string>

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

// Numeric ids are intentionally stable across builds so the WASM
// binding's setRenderOptions(panels, font_family) selector and the WebUI
// dropdown can map by integer id without reaching for a string table.
// Order: 0 antonio, 1 oswald, 2 inter, 3 sourceSerif, 4 merriweather,
// 5 bitter, 6 atkinson — matches preview.html's <option value=…>.
enum class FontFamily : uint8_t {
  kAntonio = 0,
  kOswald = 1,
  kInter = 2,
  kSourceSerif = 3,
  kMerriweather = 4,
  kBitter = 5,
  kAtkinson = 6,
};

struct FontBundle {
  const Font* regular;
  const Font* bold;
};

class AppFonts {
 public:
  AppFonts();

  // Kept callable because debug.cpp and provisioning_ui.cpp render
  // preformatted text (IP addresses, status lines) whose typography is
  // hard-coded to the body-text font. Atkinson Hyperlegible is the
  // designated body face since it's purpose-built for legibility at
  // small sizes on low-DPI displays — the role DejaVu used to fill.
  // show_custom.cpp's split-text path is locked to Oswald-bold by the
  // custom-label feature spec. Every other surface reaches for the role
  // accessors below.
  const Font& oswald_bold() const { return oswald_bold_; }
  const Font& atkinson() const { return atkinson_; }
  const Font& atkinson_bold() const { return atkinson_bold_; }
  // Material Design Icons subsetted to the few glyphs the firmware
  // paints (lightning-bolt, pickaxe, rocket-launch at the time of
  // writing). Pass the codepoints from mdi_codepoints.hpp. See
  // tools/fonts/regen_mdi.sh to add more.
  const Font& mdi() const { return mdi_; }

  // Role-based accessors. Screens should reach for these rather than the
  // named accessors above so the selectable `fontName` preference can
  // rebind families at runtime without every call site knowing which
  // family is active. `icon` and `sats_glyph` are locked to the
  // dedicated subsetted fonts — they carry glyphs the family fonts
  // don't have.
  const Font& digit() const { return *role_digit_; }
  const Font& label() const { return *role_label_; }
  const Font& small_chars() const { return *role_small_chars_; }
  const Font& unit() const { return *role_unit_; }
  const Font& icon() const { return *role_icon_; }
  const Font& sats_glyph() const { return *role_sats_glyph_; }

  // Rebind the four swappable roles (digit/label/small_chars/unit) to
  // the given family. `icon` and `sats_glyph` stay on their dedicated
  // fonts. Called from init_screen_manager at boot (reads NVS) and
  // from the PATCH /api/settings `fontName` hook at runtime.
  void SetFamily(FontFamily f);

  // Resolve a FontFamily into its (regular, bold) pair. Antonio has no
  // separate bold variant — its regular is used for both roles; callers
  // that specifically need bold markup should use Oswald or any of the
  // serif/legible families.
  FontBundle Bundle(FontFamily f) const;

 private:
  Font antonio_;
  Font oswald_;
  Font oswald_bold_;
  Font inter_;
  Font inter_bold_;
  Font source_serif_;
  Font source_serif_bold_;
  Font merriweather_;
  Font merriweather_bold_;
  Font bitter_;
  Font bitter_bold_;
  Font atkinson_;
  Font atkinson_bold_;
  Font sats_symbol_;
  Font mdi_;

  // Role bindings. Default all four swappable roles to Antonio so boot
  // before SetFamily() call is identical to the historical behaviour.
  const Font* role_digit_ = &antonio_;
  const Font* role_label_ = &antonio_;
  const Font* role_small_chars_ = &antonio_;
  const Font* role_unit_ = &antonio_;
  const Font* role_icon_ = &mdi_;
  const Font* role_sats_glyph_ = &sats_symbol_;
};

// Map the NVS `fontName` string ("antonio" / "oswald" / "inter" /
// "sourceSerif" / "merriweather" / "bitter" / "atkinson") to a
// FontFamily. Unknown values fall back to kAntonio — the day-1 default
// every built-in screen was tuned against. Devices upgrading from a
// build that stored "dejavu" land here too: kAntonio is the safe fallback.
// Defined inline so host tests can call it without linking AppFonts
// (whose ctor references TTF-blob symbols that only exist in an IDF
// build).
inline FontFamily ParseFontFamily(const std::string& id) {
  if (id == "oswald") return FontFamily::kOswald;
  if (id == "inter") return FontFamily::kInter;
  if (id == "sourceSerif") return FontFamily::kSourceSerif;
  if (id == "merriweather") return FontFamily::kMerriweather;
  if (id == "bitter") return FontFamily::kBitter;
  if (id == "atkinson") return FontFamily::kAtkinson;
  return FontFamily::kAntonio;
}

}  // namespace btclock
