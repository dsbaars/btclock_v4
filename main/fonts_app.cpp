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
      dejavu_(kDejaVuTtf, kDejaVuTtfSize),
      dejavu_bold_(kDejaVuBoldTtf, kDejaVuBoldTtfSize),
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
      dejavu_(kDejaVuTtf, SizeBetween(kDejaVuTtf, kDejaVuTtfEnd)),
      dejavu_bold_(kDejaVuBoldTtf,
                   SizeBetween(kDejaVuBoldTtf, kDejaVuBoldTtfEnd)),
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
    case FontFamily::kDejaVu:
      return {&dejavu_, &dejavu_bold_};
  }
  return {&dejavu_, &dejavu_bold_};
}

}  // namespace btclock
