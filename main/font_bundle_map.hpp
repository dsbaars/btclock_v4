// Pure-logic dispatch extracted from AppFonts::Bundle so the
// FontFamily → (regular_slot, bold_slot) mapping can be unit-tested
// without constructing real Font instances (which would need TTF blob
// symbols only present in an IDF / WASM build).
//
// The production AppFonts::Bundle in fonts_app.cpp delegates to this
// helper and then looks up the actual Font* for each slot from its
// owning storage. Tests verify the mapping table is consistent
// (no FontFamily falls through, Antonio doubles its regular into the
// bold slot, kMerriweather collapses to source-serif on Rev A).

#pragma once

#include "fonts_app.hpp"  // for FontFamily, FontSlot

namespace btclock {

struct BundleSlots {
  FontSlot regular;
  FontSlot bold;
};

// `has_merriweather` is true on Rev B / V8 (the Merriweather pair is
// embedded), false on Rev A (dropped to fit the 4 MB flash budget). On
// Rev A a stored or WebUI-set kMerriweather still resolves — it just
// substitutes the closest serif (source-serif) so the WebUI selection
// renders something serif-y instead of falling back to antonio.
inline constexpr BundleSlots ResolveBundleSlots(FontFamily f,
                                                bool has_merriweather) {
  switch (f) {
    case FontFamily::kAntonio:
      // Antonio has no separate bold in the asset set. Doubling the
      // regular into the bold slot keeps the markdown '*bold*' marker
      // parsing correctly even if the visual weight is identical.
      return {FontSlot::kAntonio, FontSlot::kAntonio};
    case FontFamily::kOswald:
      return {FontSlot::kOswaldRegular, FontSlot::kOswaldBold};
    case FontFamily::kInter:
      return {FontSlot::kInterRegular, FontSlot::kInterBold};
    case FontFamily::kSourceSerif:
      return {FontSlot::kSourceSerifRegular, FontSlot::kSourceSerifBold};
    case FontFamily::kMerriweather:
      if (!has_merriweather) {
        return {FontSlot::kSourceSerifRegular, FontSlot::kSourceSerifBold};
      }
      return {FontSlot::kMerriweatherRegular, FontSlot::kMerriweatherBold};
    case FontFamily::kBitter:
      return {FontSlot::kBitterRegular, FontSlot::kBitterBold};
    case FontFamily::kAtkinson:
      return {FontSlot::kAtkinsonRegular, FontSlot::kAtkinsonBold};
  }
  // Atkinson is the body-text fallback (legibility-first replacement
  // for the retired DejaVu pair).
  return {FontSlot::kAtkinsonRegular, FontSlot::kAtkinsonBold};
}

}  // namespace btclock
