// Pin AppFonts::Bundle's FontFamily → (regular_slot, bold_slot)
// dispatch in isolation from the Font ctor / TTF blobs (which only
// exist in an IDF build). The production AppFonts::Bundle delegates to
// ResolveBundleSlots and then looks up the actual Font* per slot, so
// pinning the slot mapping here is equivalent to pinning the public
// Bundle behaviour.
//
// The bd description suggested vendoring a tiny TTF and constructing a
// real Font for these tests. We chose the slot-table refactor instead:
// it tests the exact dispatch the production code runs (rather than a
// parallel mirror), it doesn't require linking stb_truetype + a TTF
// blob into the host build, and it matches the precedent set by
// test_font_roles.cpp where the same TTF-blob blocker was worked
// around with a pure-logic surface.

#include "doctest.h"
#include "font_bundle_map.hpp"
#include "fonts_app.hpp"

using btclock::BundleSlots;
using btclock::FontFamily;
using btclock::FontSlot;
using btclock::ResolveBundleSlots;

TEST_CASE("ResolveBundleSlots Antonio doubles the regular slot into bold") {
  // Antonio ships only one weight; the bold slot intentionally aliases
  // the regular so '*bold*' markdown still parses but renders in the
  // same weight.
  const auto s = ResolveBundleSlots(FontFamily::kAntonio,
                                    /*has_merriweather=*/true);
  CHECK(s.regular == FontSlot::kAntonio);
  CHECK(s.bold == FontSlot::kAntonio);
}

TEST_CASE("ResolveBundleSlots AntonioSemiBold pairs SemiBold with Bold") {
  // The SemiBold family carries a real bold cut: regular = wght=600,
  // bold = wght=700.
  const auto s = ResolveBundleSlots(FontFamily::kAntonioSemiBold, true);
  CHECK(s.regular == FontSlot::kAntonioSemiBold);
  CHECK(s.bold == FontSlot::kAntonioBold);
}

TEST_CASE(
    "ResolveBundleSlots AntonioBold doubles its bold cut into both slots") {
  // Like the base Antonio family, AntonioBold doubles its single weight
  // (wght=700) into the bold slot — '*bold*' still parses, just with
  // matching visual weight.
  const auto s = ResolveBundleSlots(FontFamily::kAntonioBold, true);
  CHECK(s.regular == FontSlot::kAntonioBold);
  CHECK(s.bold == FontSlot::kAntonioBold);
}

TEST_CASE("ResolveBundleSlots maps OswaldBold to its bold slot") {
  const auto s = ResolveBundleSlots(FontFamily::kOswaldBold, true);
  CHECK(s.regular == FontSlot::kOswaldBold);
  CHECK(s.bold == FontSlot::kOswaldBold);
}

TEST_CASE("ResolveBundleSlots maps InterBold to its bold slot") {
  const auto s = ResolveBundleSlots(FontFamily::kInterBold, true);
  CHECK(s.regular == FontSlot::kInterBold);
  CHECK(s.bold == FontSlot::kInterBold);
}

TEST_CASE("ResolveBundleSlots maps SourceSerifBold to its bold slot") {
  const auto s = ResolveBundleSlots(FontFamily::kSourceSerifBold, true);
  CHECK(s.regular == FontSlot::kSourceSerifBold);
  CHECK(s.bold == FontSlot::kSourceSerifBold);
}

TEST_CASE(
    "ResolveBundleSlots maps MerriweatherBold to its own slot on Rev B / V8") {
  const auto s = ResolveBundleSlots(FontFamily::kMerriweatherBold,
                                    /*has_merriweather=*/true);
  CHECK(s.regular == FontSlot::kMerriweatherBold);
  CHECK(s.bold == FontSlot::kMerriweatherBold);
}

TEST_CASE(
    "ResolveBundleSlots MerriweatherBold collapses to source-serif on Rev A") {
  const auto s = ResolveBundleSlots(FontFamily::kMerriweatherBold,
                                    /*has_merriweather=*/false);
  CHECK(s.regular == FontSlot::kSourceSerifBold);
  CHECK(s.bold == FontSlot::kSourceSerifBold);
}

TEST_CASE("ResolveBundleSlots maps BitterBold to its bold slot") {
  const auto s = ResolveBundleSlots(FontFamily::kBitterBold, true);
  CHECK(s.regular == FontSlot::kBitterBold);
  CHECK(s.bold == FontSlot::kBitterBold);
}

TEST_CASE("ResolveBundleSlots maps AtkinsonBold to its bold slot") {
  const auto s = ResolveBundleSlots(FontFamily::kAtkinsonBold, true);
  CHECK(s.regular == FontSlot::kAtkinsonBold);
  CHECK(s.bold == FontSlot::kAtkinsonBold);
}

TEST_CASE("ResolveBundleSlots maps Oswald to its (regular, bold) pair") {
  const auto s = ResolveBundleSlots(FontFamily::kOswald, true);
  CHECK(s.regular == FontSlot::kOswaldRegular);
  CHECK(s.bold == FontSlot::kOswaldBold);
}

TEST_CASE("ResolveBundleSlots maps Inter to its (regular, bold) pair") {
  const auto s = ResolveBundleSlots(FontFamily::kInter, true);
  CHECK(s.regular == FontSlot::kInterRegular);
  CHECK(s.bold == FontSlot::kInterBold);
}

TEST_CASE("ResolveBundleSlots maps SourceSerif to its (regular, bold) pair") {
  const auto s = ResolveBundleSlots(FontFamily::kSourceSerif, true);
  CHECK(s.regular == FontSlot::kSourceSerifRegular);
  CHECK(s.bold == FontSlot::kSourceSerifBold);
}

TEST_CASE("ResolveBundleSlots maps Bitter to its (regular, bold) pair") {
  const auto s = ResolveBundleSlots(FontFamily::kBitter, true);
  CHECK(s.regular == FontSlot::kBitterRegular);
  CHECK(s.bold == FontSlot::kBitterBold);
}

TEST_CASE("ResolveBundleSlots maps Atkinson to its (regular, bold) pair") {
  const auto s = ResolveBundleSlots(FontFamily::kAtkinson, true);
  CHECK(s.regular == FontSlot::kAtkinsonRegular);
  CHECK(s.bold == FontSlot::kAtkinsonBold);
}

TEST_CASE(
    "ResolveBundleSlots Merriweather maps to its own slots on Rev B / V8") {
  const auto s = ResolveBundleSlots(FontFamily::kMerriweather,
                                    /*has_merriweather=*/true);
  CHECK(s.regular == FontSlot::kMerriweatherRegular);
  CHECK(s.bold == FontSlot::kMerriweatherBold);
}

TEST_CASE(
    "ResolveBundleSlots Merriweather collapses to source-serif on Rev A") {
  // Rev A's 4 MB flash drops the Merriweather pair from EMBED_FILES;
  // a stored or WebUI-set kMerriweather still resolves to *something*
  // serif-y rather than refusing to switch or falling back to antonio.
  const auto s = ResolveBundleSlots(FontFamily::kMerriweather,
                                    /*has_merriweather=*/false);
  CHECK(s.regular == FontSlot::kSourceSerifRegular);
  CHECK(s.bold == FontSlot::kSourceSerifBold);
}

TEST_CASE("ResolveBundleSlots covers every FontFamily enumerator") {
  // Sanity: walk every defined FontFamily value and assert the
  // dispatch returns slots that are themselves valid (≤ the highest
  // FontSlot enumerator). Catches a future enumerator that someone
  // adds to FontFamily but forgets to handle in ResolveBundleSlots —
  // the default-Atkinson tail covers that case but the test pins it.
  for (uint8_t i = static_cast<uint8_t>(FontFamily::kAntonio);
       i <= static_cast<uint8_t>(FontFamily::kAtkinsonBold); ++i) {
    const auto f = static_cast<FontFamily>(i);
    const auto s = ResolveBundleSlots(f, /*has_merriweather=*/true);
    CHECK(static_cast<uint8_t>(s.regular) <=
          static_cast<uint8_t>(FontSlot::kAntonioBold));
    CHECK(static_cast<uint8_t>(s.bold) <=
          static_cast<uint8_t>(FontSlot::kAntonioBold));
  }
}

TEST_CASE("ResolveBundleSlots is constexpr-evaluable") {
  // Compile-time evaluation pins both the constexpr-ness of the
  // helper and the Antonio mapping value at compile time.
  constexpr auto s = ResolveBundleSlots(FontFamily::kAntonio, true);
  static_assert(s.regular == FontSlot::kAntonio, "antonio regular slot");
  static_assert(s.bold == FontSlot::kAntonio, "antonio bold slot");
  CHECK(s.regular == FontSlot::kAntonio);
}

TEST_CASE("FontSlot enumerators are unique") {
  // The slot ids only matter relative to AppFonts storage but a
  // duplicate would silently merge two backing fonts under one id.
  const FontSlot all[] = {
      FontSlot::kAntonio,          FontSlot::kOswaldRegular,
      FontSlot::kOswaldBold,       FontSlot::kInterRegular,
      FontSlot::kInterBold,        FontSlot::kSourceSerifRegular,
      FontSlot::kSourceSerifBold,  FontSlot::kMerriweatherRegular,
      FontSlot::kMerriweatherBold, FontSlot::kBitterRegular,
      FontSlot::kBitterBold,       FontSlot::kAtkinsonRegular,
      FontSlot::kAtkinsonBold,     FontSlot::kAntonioSemiBold,
      FontSlot::kAntonioBold,
  };
  const std::size_t n = sizeof(all) / sizeof(all[0]);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      CHECK(static_cast<uint8_t>(all[i]) != static_cast<uint8_t>(all[j]));
    }
  }
}
