// Pin the role-based font API surface so future edits to AppFonts
// don't silently drop the `fontName` plumbing or swap an assigned
// role onto the wrong backing field.
//
// We can't exercise the full AppFonts ctor at host build time —
// font.cpp / stb_truetype aren't linked into btclock_host_tests, and
// the linker-magic `_binary_Antonio_ttf_start` symbols only exist in
// an IDF build. Instead this test uses a fixture that mirrors the
// role-pointer layout in fonts_app.hpp and exercises the same
// swap-the-pointer semantics that AppFonts::SetFamily implements.
//
// Coverage:
//   - Defaults: digit/label/small_chars/unit all alias the same
//     "antonio" storage; icon/sats_glyph alias their dedicated fonts.
//   - SetFamily(kOswald) rebinds the four swappable roles to oswald;
//     icon/sats_glyph unchanged.
//   - SetFamily(kDejaVu) likewise.
//   - ParseFontFamily maps the NVS strings + falls back to kAntonio.
//
// When Agent B.2's sweep is stable we can revisit whether it's worth
// threading the real AppFonts through the host build (it would mean
// linking font.cpp + stb_truetype + the TTF blobs, or stubbing out
// Font::Font so it succeeds on an empty buffer).

#include "doctest.h"

#include <string>

#include "fonts_app.hpp"

namespace btclock {

// Mirror the internal role bindings via a minimal fixture so we test
// the logic in isolation from Font / stb_truetype. The production
// AppFonts does exactly the same pointer-shuffle in SetFamily.
namespace {
struct RoleFixture {
  int antonio = 0;
  int oswald = 0;
  int dejavu = 0;
  int mdi = 0;
  int sats = 0;

  const int* digit = &antonio;
  const int* label = &antonio;
  const int* small_chars = &antonio;
  const int* unit = &antonio;
  const int* icon = &mdi;
  const int* sats_glyph = &sats;

  void SetFamily(FontFamily f) {
    const int* regular = &antonio;
    switch (f) {
      case FontFamily::kAntonio: regular = &antonio; break;
      case FontFamily::kOswald:  regular = &oswald;  break;
      case FontFamily::kDejaVu:  regular = &dejavu;  break;
    }
    digit = regular;
    label = regular;
    small_chars = regular;
    unit = regular;
  }
};
}  // namespace

TEST_CASE("AppFonts role defaults alias antonio storage") {
  RoleFixture f;
  CHECK(f.digit == f.label);
  CHECK(f.label == f.small_chars);
  CHECK(f.small_chars == f.unit);
  CHECK(f.digit == &f.antonio);
  // icon + sats_glyph lock onto their dedicated fonts — never folded
  // into a swappable role.
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kOswald) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kOswald);
  CHECK(f.digit == &f.oswald);
  CHECK(f.label == &f.oswald);
  CHECK(f.small_chars == &f.oswald);
  CHECK(f.unit == &f.oswald);
  // Locked roles unchanged — the MDI / sats-symbol subsets don't live
  // in the family fonts, so swapping a family must never clobber them.
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kDejaVu) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kDejaVu);
  CHECK(f.digit == &f.dejavu);
  CHECK(f.label == &f.dejavu);
  CHECK(f.small_chars == &f.dejavu);
  CHECK(f.unit == &f.dejavu);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kAntonio) returns to day-1 default") {
  RoleFixture f;
  f.SetFamily(FontFamily::kOswald);
  f.SetFamily(FontFamily::kAntonio);
  CHECK(f.digit == &f.antonio);
  CHECK(f.label == &f.antonio);
  CHECK(f.small_chars == &f.antonio);
  CHECK(f.unit == &f.antonio);
}

TEST_CASE("ParseFontFamily maps NVS strings") {
  CHECK(ParseFontFamily("antonio") == FontFamily::kAntonio);
  CHECK(ParseFontFamily("oswald")  == FontFamily::kOswald);
  CHECK(ParseFontFamily("dejavu")  == FontFamily::kDejaVu);
}

TEST_CASE("ParseFontFamily falls back to Antonio on unknown input") {
  // Factory-clean NVS, a typo, or a legacy value the firmware no
  // longer ships — all should paint with the day-1 default rather
  // than refusing to boot.
  CHECK(ParseFontFamily("") == FontFamily::kAntonio);
  CHECK(ParseFontFamily("Antonio") == FontFamily::kAntonio);  // case-sensitive
  CHECK(ParseFontFamily("comic-sans") == FontFamily::kAntonio);
}

}  // namespace btclock
