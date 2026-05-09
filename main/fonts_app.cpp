#include "fonts_app.hpp"

#include <cstddef>

#include "font_bundle_map.hpp"

namespace btclock {

#ifdef BTCLOCK_WASM_BUILD
// Under WASM the generator emits explicit array + size variables, so
// we just pass them straight through.
AppFonts::AppFonts()
    : antonio_(kAntonioTtf, kAntonioTtfSize),
      antonio_semibold_(kAntonioSemiBoldTtf, kAntonioSemiBoldTtfSize),
      antonio_bold_(kAntonioBoldTtf, kAntonioBoldTtfSize),
      oswald_(kOswaldTtf, kOswaldTtfSize),
      oswald_bold_(kOswaldBoldTtf, kOswaldBoldTtfSize),
      inter_(kInterTtf, kInterTtfSize),
      inter_bold_(kInterBoldTtf, kInterBoldTtfSize),
      open_runde_(kOpenRundeTtf, kOpenRundeTtfSize),
      open_runde_bold_(kOpenRundeBoldTtf, kOpenRundeBoldTtfSize),
      source_serif_(kSourceSerifTtf, kSourceSerifTtfSize),
      source_serif_bold_(kSourceSerifBoldTtf, kSourceSerifBoldTtfSize),
#ifndef BTCLOCK_BOARD_REV_A
      merriweather_(kMerriweatherTtf, kMerriweatherTtfSize),
      merriweather_bold_(kMerriweatherBoldTtf, kMerriweatherBoldTtfSize),
#endif
      bitter_(kBitterTtf, kBitterTtfSize),
      bitter_bold_(kBitterBoldTtf, kBitterBoldTtfSize),
      atkinson_(kAtkinsonTtf, kAtkinsonTtfSize),
      atkinson_bold_(kAtkinsonBoldTtf, kAtkinsonBoldTtfSize),
      sats_symbol_(kSatoshiSymbolTtf, kSatoshiSymbolTtfSize),
      mdi_(kMaterialDesignIconsTtf, kMaterialDesignIconsTtfSize) {
}
#else
namespace {
size_t SizeBetween(const uint8_t* a, const uint8_t* b) {
  return static_cast<size_t>(b - a);
}
}  // namespace

AppFonts::AppFonts()
    : antonio_(kAntonioTtf, SizeBetween(kAntonioTtf, kAntonioTtfEnd)),
      antonio_semibold_(
          kAntonioSemiBoldTtf,
          SizeBetween(kAntonioSemiBoldTtf, kAntonioSemiBoldTtfEnd)),
      antonio_bold_(kAntonioBoldTtf,
                    SizeBetween(kAntonioBoldTtf, kAntonioBoldTtfEnd)),
      oswald_(kOswaldTtf, SizeBetween(kOswaldTtf, kOswaldTtfEnd)),
      oswald_bold_(kOswaldBoldTtf,
                   SizeBetween(kOswaldBoldTtf, kOswaldBoldTtfEnd)),
      inter_(kInterTtf, SizeBetween(kInterTtf, kInterTtfEnd)),
      inter_bold_(kInterBoldTtf, SizeBetween(kInterBoldTtf, kInterBoldTtfEnd)),
      open_runde_(kOpenRundeTtf, SizeBetween(kOpenRundeTtf, kOpenRundeTtfEnd)),
      open_runde_bold_(kOpenRundeBoldTtf,
                       SizeBetween(kOpenRundeBoldTtf, kOpenRundeBoldTtfEnd)),
      source_serif_(kSourceSerifTtf,
                    SizeBetween(kSourceSerifTtf, kSourceSerifTtfEnd)),
      source_serif_bold_(
          kSourceSerifBoldTtf,
          SizeBetween(kSourceSerifBoldTtf, kSourceSerifBoldTtfEnd)),
#ifndef BTCLOCK_BOARD_REV_A
      merriweather_(kMerriweatherTtf,
                    SizeBetween(kMerriweatherTtf, kMerriweatherTtfEnd)),
      merriweather_bold_(
          kMerriweatherBoldTtf,
          SizeBetween(kMerriweatherBoldTtf, kMerriweatherBoldTtfEnd)),
#endif
      bitter_(kBitterTtf, SizeBetween(kBitterTtf, kBitterTtfEnd)),
      bitter_bold_(kBitterBoldTtf,
                   SizeBetween(kBitterBoldTtf, kBitterBoldTtfEnd)),
      atkinson_(kAtkinsonTtf, SizeBetween(kAtkinsonTtf, kAtkinsonTtfEnd)),
      atkinson_bold_(kAtkinsonBoldTtf,
                     SizeBetween(kAtkinsonBoldTtf, kAtkinsonBoldTtfEnd)),
      sats_symbol_(kSatoshiSymbolTtf,
                   SizeBetween(kSatoshiSymbolTtf, kSatoshiSymbolTtfEnd)),
      mdi_(kMaterialDesignIconsTtf,
           SizeBetween(kMaterialDesignIconsTtf, kMaterialDesignIconsTtfEnd)) {
}
#endif

FontBundle AppFonts::Bundle(FontFamily f) const {
  // Rev A drops the Merriweather pair from EMBED_FILES (4 MB flash
  // budget) — see ResolveBundleSlots's substitution branch.
#ifdef BTCLOCK_BOARD_REV_A
  constexpr bool kHasMerriweather = false;
#else
  constexpr bool kHasMerriweather = true;
#endif
  const auto slots = ResolveBundleSlots(f, kHasMerriweather);
  return {SlotToFont(slots.regular), SlotToFont(slots.bold)};
}

const Font* AppFonts::SlotToFont(FontSlot s) const {
  switch (s) {
    case FontSlot::kAntonio:
      return &antonio_;
    case FontSlot::kAntonioSemiBold:
      return &antonio_semibold_;
    case FontSlot::kAntonioBold:
      return &antonio_bold_;
    case FontSlot::kOswaldRegular:
      return &oswald_;
    case FontSlot::kOswaldBold:
      return &oswald_bold_;
    case FontSlot::kInterRegular:
      return &inter_;
    case FontSlot::kInterBold:
      return &inter_bold_;
    case FontSlot::kSourceSerifRegular:
      return &source_serif_;
    case FontSlot::kSourceSerifBold:
      return &source_serif_bold_;
    case FontSlot::kMerriweatherRegular:
#ifdef BTCLOCK_BOARD_REV_A
      // Slot is unreachable on Rev A — ResolveBundleSlots redirects
      // kMerriweather to the source-serif slots before we get here.
      return &source_serif_;
#else
      return &merriweather_;
#endif
    case FontSlot::kMerriweatherBold:
#ifdef BTCLOCK_BOARD_REV_A
      return &source_serif_bold_;
#else
      return &merriweather_bold_;
#endif
    case FontSlot::kBitterRegular:
      return &bitter_;
    case FontSlot::kBitterBold:
      return &bitter_bold_;
    case FontSlot::kAtkinsonRegular:
      return &atkinson_;
    case FontSlot::kAtkinsonBold:
      return &atkinson_bold_;
    case FontSlot::kOpenRundeRegular:
      return &open_runde_;
    case FontSlot::kOpenRundeBold:
      return &open_runde_bold_;
  }
  return &atkinson_;
}

void AppFonts::SetFamily(FontFamily f) {
  const Font* regular = &antonio_;
  switch (f) {
    case FontFamily::kAntonio:
      regular = &antonio_;
      break;
    case FontFamily::kAntonioSemiBold:
      regular = &antonio_semibold_;
      break;
    case FontFamily::kAntonioBold:
      regular = &antonio_bold_;
      break;
    case FontFamily::kOswaldBold:
      regular = &oswald_bold_;
      break;
    case FontFamily::kInterBold:
      regular = &inter_bold_;
      break;
    case FontFamily::kSourceSerifBold:
      regular = &source_serif_bold_;
      break;
    case FontFamily::kMerriweatherBold:
#ifdef BTCLOCK_BOARD_REV_A
      regular = &source_serif_bold_;
#else
      regular = &merriweather_bold_;
#endif
      break;
    case FontFamily::kBitterBold:
      regular = &bitter_bold_;
      break;
    case FontFamily::kAtkinsonBold:
      regular = &atkinson_bold_;
      break;
    case FontFamily::kOpenRunde:
      regular = &open_runde_;
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
#ifdef BTCLOCK_BOARD_REV_A
      regular = &source_serif_;
#else
      regular = &merriweather_;
#endif
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
