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
//   - Same coverage for kInter, kSourceSerif, kMerriweather, kBitter,
//     kAtkinson.
//   - ParseFontFamily maps the NVS strings + falls back to kAntonio.
//
// When Agent B.2's sweep is stable we can revisit whether it's worth
// threading the real AppFonts through the host build (it would mean
// linking font.cpp + stb_truetype + the TTF blobs, or stubbing out
// Font::Font so it succeeds on an empty buffer).

#include <string>

#include "doctest.h"
#include "fonts_app.hpp"

namespace btclock {

// Mirror the internal role bindings via a minimal fixture so we test
// the logic in isolation from Font / stb_truetype. The production
// AppFonts does exactly the same pointer-shuffle in SetFamily.
namespace {
struct RoleFixture {
  int antonio = 0;
  int antonio_semibold = 0;
  int antonio_bold = 0;
  int oswald = 0;
  int oswald_bold = 0;
  int inter = 0;
  int inter_bold = 0;
  int source_serif = 0;
  int source_serif_bold = 0;
  int merriweather = 0;
  int merriweather_bold = 0;
  int bitter = 0;
  int bitter_bold = 0;
  int atkinson = 0;
  int atkinson_bold = 0;
  int open_runde = 0;
  int open_runde_bold = 0;
  int roboto = 0;
  int roboto_bold = 0;
  int noto_sans = 0;
  int noto_sans_bold = 0;
  int ubuntu = 0;
  int ubuntu_bold = 0;
  int azeret = 0;
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
      case FontFamily::kAntonio:
        regular = &antonio;
        break;
      case FontFamily::kAntonioSemiBold:
        regular = &antonio_semibold;
        break;
      case FontFamily::kAntonioBold:
        regular = &antonio_bold;
        break;
      case FontFamily::kOswaldBold:
        regular = &oswald_bold;
        break;
      case FontFamily::kInterBold:
        regular = &inter_bold;
        break;
      case FontFamily::kSourceSerifBold:
        regular = &source_serif_bold;
        break;
      case FontFamily::kMerriweatherBold:
        regular = &merriweather_bold;
        break;
      case FontFamily::kBitterBold:
        regular = &bitter_bold;
        break;
      case FontFamily::kAtkinsonBold:
        regular = &atkinson_bold;
        break;
      case FontFamily::kOpenRunde:
        regular = &open_runde;
        break;
      case FontFamily::kRobotoBold:
        regular = &roboto_bold;
        break;
      case FontFamily::kNotoSansBold:
        regular = &noto_sans_bold;
        break;
      case FontFamily::kUbuntuBold:
        regular = &ubuntu_bold;
        break;
      case FontFamily::kOswald:
        regular = &oswald;
        break;
      case FontFamily::kInter:
        regular = &inter;
        break;
      case FontFamily::kSourceSerif:
        regular = &source_serif;
        break;
      case FontFamily::kMerriweather:
        regular = &merriweather;
        break;
      case FontFamily::kBitter:
        regular = &bitter;
        break;
      case FontFamily::kAtkinson:
        regular = &atkinson;
        break;
      case FontFamily::kRoboto:
        regular = &roboto;
        break;
      case FontFamily::kNotoSans:
        regular = &noto_sans;
        break;
      case FontFamily::kUbuntu:
        regular = &ubuntu;
        break;
      case FontFamily::kAzeret:
        regular = &azeret;
        break;
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

TEST_CASE("SetFamily(kInter) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kInter);
  CHECK(f.digit == &f.inter);
  CHECK(f.label == &f.inter);
  CHECK(f.small_chars == &f.inter);
  CHECK(f.unit == &f.inter);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kSourceSerif) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kSourceSerif);
  CHECK(f.digit == &f.source_serif);
  CHECK(f.label == &f.source_serif);
  CHECK(f.small_chars == &f.source_serif);
  CHECK(f.unit == &f.source_serif);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kMerriweather) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kMerriweather);
  CHECK(f.digit == &f.merriweather);
  CHECK(f.label == &f.merriweather);
  CHECK(f.small_chars == &f.merriweather);
  CHECK(f.unit == &f.merriweather);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kBitter) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kBitter);
  CHECK(f.digit == &f.bitter);
  CHECK(f.label == &f.bitter);
  CHECK(f.small_chars == &f.bitter);
  CHECK(f.unit == &f.bitter);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kAtkinson) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kAtkinson);
  CHECK(f.digit == &f.atkinson);
  CHECK(f.label == &f.atkinson);
  CHECK(f.small_chars == &f.atkinson);
  CHECK(f.unit == &f.atkinson);
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
  CHECK(ParseFontFamily("antonioSemiBold") == FontFamily::kAntonioSemiBold);
  CHECK(ParseFontFamily("antonioBold") == FontFamily::kAntonioBold);
  CHECK(ParseFontFamily("oswald") == FontFamily::kOswald);
  CHECK(ParseFontFamily("oswaldBold") == FontFamily::kOswaldBold);
  CHECK(ParseFontFamily("inter") == FontFamily::kInter);
  CHECK(ParseFontFamily("interBold") == FontFamily::kInterBold);
  CHECK(ParseFontFamily("sourceSerif") == FontFamily::kSourceSerif);
  CHECK(ParseFontFamily("sourceSerifBold") == FontFamily::kSourceSerifBold);
  CHECK(ParseFontFamily("merriweather") == FontFamily::kMerriweather);
  CHECK(ParseFontFamily("merriweatherBold") == FontFamily::kMerriweatherBold);
  CHECK(ParseFontFamily("bitter") == FontFamily::kBitter);
  CHECK(ParseFontFamily("bitterBold") == FontFamily::kBitterBold);
  CHECK(ParseFontFamily("atkinson") == FontFamily::kAtkinson);
  CHECK(ParseFontFamily("atkinsonBold") == FontFamily::kAtkinsonBold);
  CHECK(ParseFontFamily("openRunde") == FontFamily::kOpenRunde);
  CHECK(ParseFontFamily("roboto") == FontFamily::kRoboto);
  CHECK(ParseFontFamily("robotoBold") == FontFamily::kRobotoBold);
  CHECK(ParseFontFamily("notoSans") == FontFamily::kNotoSans);
  CHECK(ParseFontFamily("notoSansBold") == FontFamily::kNotoSansBold);
  CHECK(ParseFontFamily("ubuntu") == FontFamily::kUbuntu);
  CHECK(ParseFontFamily("ubuntuBold") == FontFamily::kUbuntuBold);
  CHECK(ParseFontFamily("azeret") == FontFamily::kAzeret);
  // Retired family — falls through to kAntonio so upgrading devices
  // with "azeretSemiBold" stored in NVS keep booting.
  CHECK(ParseFontFamily("azeretSemiBold") == FontFamily::kAntonio);
}

TEST_CASE("SetFamily(kAntonioSemiBold) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kAntonioSemiBold);
  CHECK(f.digit == &f.antonio_semibold);
  CHECK(f.label == &f.antonio_semibold);
  CHECK(f.small_chars == &f.antonio_semibold);
  CHECK(f.unit == &f.antonio_semibold);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kAntonioBold) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kAntonioBold);
  CHECK(f.digit == &f.antonio_bold);
  CHECK(f.label == &f.antonio_bold);
  CHECK(f.small_chars == &f.antonio_bold);
  CHECK(f.unit == &f.antonio_bold);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kInterBold) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kInterBold);
  CHECK(f.digit == &f.inter_bold);
  CHECK(f.label == &f.inter_bold);
  CHECK(f.small_chars == &f.inter_bold);
  CHECK(f.unit == &f.inter_bold);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kOpenRunde) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kOpenRunde);
  CHECK(f.digit == &f.open_runde);
  CHECK(f.label == &f.open_runde);
  CHECK(f.small_chars == &f.open_runde);
  CHECK(f.unit == &f.open_runde);
  CHECK(f.icon == &f.mdi);
  CHECK(f.sats_glyph == &f.sats);
}

TEST_CASE("SetFamily(kRoboto) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kRoboto);
  CHECK(f.digit == &f.roboto);
  CHECK(f.label == &f.roboto);
  CHECK(f.small_chars == &f.roboto);
  CHECK(f.unit == &f.roboto);
}

TEST_CASE("SetFamily(kNotoSans) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kNotoSans);
  CHECK(f.digit == &f.noto_sans);
  CHECK(f.label == &f.noto_sans);
  CHECK(f.small_chars == &f.noto_sans);
  CHECK(f.unit == &f.noto_sans);
}

TEST_CASE("SetFamily(kUbuntu) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kUbuntu);
  CHECK(f.digit == &f.ubuntu);
  CHECK(f.label == &f.ubuntu);
  CHECK(f.small_chars == &f.ubuntu);
  CHECK(f.unit == &f.ubuntu);
}

TEST_CASE("SetFamily(kAzeret) rebinds swappable roles") {
  RoleFixture f;
  f.SetFamily(FontFamily::kAzeret);
  CHECK(f.digit == &f.azeret);
  CHECK(f.label == &f.azeret);
  CHECK(f.small_chars == &f.azeret);
  CHECK(f.unit == &f.azeret);
}

TEST_CASE("ParseFontFamily falls back to Antonio on unknown input") {
  // Factory-clean NVS, a typo, or a legacy value the firmware no
  // longer ships — all should paint with the day-1 default rather
  // than refusing to boot.
  CHECK(ParseFontFamily("") == FontFamily::kAntonio);
  CHECK(ParseFontFamily("Antonio") == FontFamily::kAntonio);  // case-sensitive
  CHECK(ParseFontFamily("comic-sans") == FontFamily::kAntonio);
  // Devices upgrading from an older firmware that stored the retired
  // "dejavu" family should land on the antonio default rather than
  // refusing to boot or crashing on a missing enum case.
  CHECK(ParseFontFamily("dejavu") == FontFamily::kAntonio);
}

}  // namespace btclock
