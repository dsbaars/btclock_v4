// Bitaxe logo bitmap.
//
// Single 1-bpp MSB-first bitmap vendored from the old btclock_v3_fci
// firmware (src/icons/icons.cpp, `epd_icons_bitaxe_logo`, 88×220). The
// bitaxe screens paint this on panel 0 instead of a "BIT" / "AXE" text
// split — matches the miner's visual identity and frees panel 1 for an
// extra digit slot.
//
// Format note: a 0 bit is ink (black pixel painted into the landscape
// framebuffer), a 1 bit is background — same semantics as
// pool_logos.cpp's `PaintInvertedBitmap` consumes. Kept as a plain
// extern symbol rather than a lookup registry because there's only one
// bitaxe logo; no case-folding or key match needed.

#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {
namespace bitaxe_logo {

inline constexpr int kWidth = 88;
inline constexpr int kHeight = 220;

extern const std::uint8_t kBitmap[];
extern const std::size_t kBitmapSize;

}  // namespace bitaxe_logo
}  // namespace btclock
