#pragma once

#include <cstdint>
#include <string_view>

namespace btclock {

// Pixel height for MDI glyphs pushed via POST /api/show/custom cells that
// use the `mdi:<name>` pattern. Matches PaintSlot::kMdiIcon sizing in
// screens/common.cpp (kSatsGlyphPx).
constexpr float kMdiCustomCellPixelPx = 130.0f;

// Recognizes trimmed custom cells `mdi:<icon>` where <icon> is the MDI
// webfont name in kebab-case — the same tokens listed in
// tools/fonts/regen_mdi.sh (subset must contain the glyph).
//
// Returns false → caller renders plain or split text as today.
// Returns true → cell was consumed as an mdi directive; `out_codepoint`
// is 0 for `mdi:` with no name or an unknown name (panel stays blank),
// else the Unicode scalar for DrawCodepointCentered + fonts.icon().
//
// The prefix `mdi:` is matched ASCII case-insensitively. `/` in the
// name rejects mdi mode so mixed cells fall through to split-text logic.
bool ParseCustomCellMdi(std::string_view cell, std::uint32_t* out_codepoint);

}  // namespace btclock
