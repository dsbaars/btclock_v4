#include "fonts_app.hpp"

#include <cstddef>

namespace btclock {

#ifdef BTCLOCK_WASM_BUILD
// Under WASM the generator emits explicit array + size variables, so
// we just pass them straight through.
AppFonts::AppFonts()
    : antonio_(kAntonioTtf, kAntonioTtfSize),
      oswald_(kOswaldTtf, kOswaldTtfSize),
      oswald_bold_(kOswaldBoldTtf, kOswaldBoldTtfSize),
      inter_(kInterTtf, kInterTtfSize),
      inter_bold_(kInterBoldTtf, kInterBoldTtfSize),
      source_serif_(kSourceSerifTtf, kSourceSerifTtfSize),
      source_serif_bold_(kSourceSerifBoldTtf, kSourceSerifBoldTtfSize),
      merriweather_(kMerriweatherTtf, kMerriweatherTtfSize),
      merriweather_bold_(kMerriweatherBoldTtf, kMerriweatherBoldTtfSize),
      bitter_(kBitterTtf, kBitterTtfSize),
      bitter_bold_(kBitterBoldTtf, kBitterBoldTtfSize),
      atkinson_(kAtkinsonTtf, kAtkinsonTtfSize),
      atkinson_bold_(kAtkinsonBoldTtf, kAtkinsonBoldTtfSize),
      sats_symbol_(kSatoshiSymbolTtf, kSatoshiSymbolTtfSize),
      mdi_(kMaterialDesignIconsTtf, kMaterialDesignIconsTtfSize) {}
#else
namespace {
size_t SizeBetween(const uint8_t* a, const uint8_t* b) {
  return static_cast<size_t>(b - a);
}
}  // namespace

AppFonts::AppFonts()
    : antonio_(kAntonioTtf, SizeBetween(kAntonioTtf, kAntonioTtfEnd)),
      oswald_(kOswaldTtf, SizeBetween(kOswaldTtf, kOswaldTtfEnd)),
      oswald_bold_(kOswaldBoldTtf,
                   SizeBetween(kOswaldBoldTtf, kOswaldBoldTtfEnd)),
      inter_(kInterTtf,
             SizeBetween(kInterTtf, kInterTtfEnd)),
      inter_bold_(
          kInterBoldTtf,
          SizeBetween(kInterBoldTtf, kInterBoldTtfEnd)),
      source_serif_(kSourceSerifTtf,
                    SizeBetween(kSourceSerifTtf, kSourceSerifTtfEnd)),
      source_serif_bold_(
          kSourceSerifBoldTtf,
          SizeBetween(kSourceSerifBoldTtf, kSourceSerifBoldTtfEnd)),
      merriweather_(kMerriweatherTtf,
                    SizeBetween(kMerriweatherTtf, kMerriweatherTtfEnd)),
      merriweather_bold_(
          kMerriweatherBoldTtf,
          SizeBetween(kMerriweatherBoldTtf, kMerriweatherBoldTtfEnd)),
      bitter_(kBitterTtf, SizeBetween(kBitterTtf, kBitterTtfEnd)),
      bitter_bold_(kBitterBoldTtf,
                   SizeBetween(kBitterBoldTtf, kBitterBoldTtfEnd)),
      atkinson_(kAtkinsonTtf, SizeBetween(kAtkinsonTtf, kAtkinsonTtfEnd)),
      atkinson_bold_(kAtkinsonBoldTtf,
                     SizeBetween(kAtkinsonBoldTtf, kAtkinsonBoldTtfEnd)),
      sats_symbol_(kSatoshiSymbolTtf,
                   SizeBetween(kSatoshiSymbolTtf, kSatoshiSymbolTtfEnd)),
      mdi_(kMaterialDesignIconsTtf,
           SizeBetween(kMaterialDesignIconsTtf, kMaterialDesignIconsTtfEnd)) {}
#endif

FontBundle AppFonts::Bundle(FontFamily f) const {
  switch (f) {
    case FontFamily::kAntonio:
      // Antonio has no separate bold in the current asset set. Use
      // Antonio for both slots — the markdown '*bold*' marker still
      // parses but renders in the same weight.
      return {&antonio_, &antonio_};
    case FontFamily::kOswald:
      return {&oswald_, &oswald_bold_};
    case FontFamily::kInter:
      return {&inter_, &inter_bold_};
    case FontFamily::kSourceSerif:
      return {&source_serif_, &source_serif_bold_};
    case FontFamily::kMerriweather:
      return {&merriweather_, &merriweather_bold_};
    case FontFamily::kBitter:
      return {&bitter_, &bitter_bold_};
    case FontFamily::kAtkinson:
      return {&atkinson_, &atkinson_bold_};
  }
  // Atkinson is the body-text fallback (legibility-first replacement
  // for the retired DejaVu pair).
  return {&atkinson_, &atkinson_bold_};
}

void AppFonts::SetFamily(FontFamily f) {
  const Font* regular = &antonio_;
  switch (f) {
    case FontFamily::kAntonio:
      regular = &antonio_;
      break;
    case FontFamily::kOswald:
      regular = &oswald_;
      break;
    case FontFamily::kInter:
      regular = &inter_;
      break;
    case FontFamily::kSourceSerif:
      regular = &source_serif_;
      break;
    case FontFamily::kMerriweather:
      regular = &merriweather_;
      break;
    case FontFamily::kBitter:
      regular = &bitter_;
      break;
    case FontFamily::kAtkinson:
      regular = &atkinson_;
      break;
  }
  role_digit_ = regular;
  role_label_ = regular;
  role_small_chars_ = regular;
  role_unit_ = regular;
  // icon and sats_glyph stay locked — the family fonts don't carry the
  // MDI PUA codepoints or the subsetted 'S' glyph.
}

}  // namespace btclock
