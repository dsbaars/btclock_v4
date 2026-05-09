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
      // The base Antonio family carries only the wght=400 cut. Doubling
      // the regular into the bold slot keeps the markdown '*bold*'
      // marker parsing correctly even if the visual weight is identical.
      // Users who want a real bold markup contrast should pick
      // kAntonioSemiBold (regular=600, bold=700) instead.
      return {FontSlot::kAntonio, FontSlot::kAntonio};
    case FontFamily::kAntonioSemiBold:
      // SemiBold (wght=600) for body, Bold (wght=700) for markdown
      // '*bold*' — the 100-unit weight delta gives noticeable contrast
      // at the panel sizes the renderer uses.
      return {FontSlot::kAntonioSemiBold, FontSlot::kAntonioBold};
    case FontFamily::kAntonioBold:
      // Bold (wght=700) doubled into the bold slot. Like the base
      // Antonio family this collapses '*bold*' visually but keeps the
      // markdown parser happy.
      return {FontSlot::kAntonioBold, FontSlot::kAntonioBold};
    case FontFamily::kOswaldBold:
      return {FontSlot::kOswaldBold, FontSlot::kOswaldBold};
    case FontFamily::kInterBold:
      return {FontSlot::kInterBold, FontSlot::kInterBold};
    case FontFamily::kSourceSerifBold:
      return {FontSlot::kSourceSerifBold, FontSlot::kSourceSerifBold};
    case FontFamily::kMerriweatherBold:
      if (!has_merriweather)
        return {FontSlot::kSourceSerifBold, FontSlot::kSourceSerifBold};
      return {FontSlot::kMerriweatherBold, FontSlot::kMerriweatherBold};
    case FontFamily::kBitterBold:
      return {FontSlot::kBitterBold, FontSlot::kBitterBold};
    case FontFamily::kAtkinsonBold:
      return {FontSlot::kAtkinsonBold, FontSlot::kAtkinsonBold};
    case FontFamily::kRobotoBold:
      return {FontSlot::kRobotoBold, FontSlot::kRobotoBold};
    case FontFamily::kNotoSansBold:
      return {FontSlot::kNotoSansBold, FontSlot::kNotoSansBold};
    case FontFamily::kUbuntuBold:
      return {FontSlot::kUbuntuBold, FontSlot::kUbuntuBold};
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
    case FontFamily::kOpenRunde:
      return {FontSlot::kOpenRundeRegular, FontSlot::kOpenRundeBold};
    case FontFamily::kRoboto:
      return {FontSlot::kRobotoRegular, FontSlot::kRobotoBold};
    case FontFamily::kNotoSans:
      return {FontSlot::kNotoSansRegular, FontSlot::kNotoSansBold};
    case FontFamily::kUbuntu:
      return {FontSlot::kUbuntuRegular, FontSlot::kUbuntuBold};
  }
  // Atkinson is the body-text fallback (legibility-first replacement
  // for the retired DejaVu pair).
  return {FontSlot::kAtkinsonRegular, FontSlot::kAtkinsonBold};
}

}  // namespace btclock
