// Private helpers shared across screen renderers.
//
// Not part of the public screens API — only the per-screen .cpp files
// include this. Keep it header-only where it matters (tiny templates /
// constants) and push non-template impls into common.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Under the emscripten/WASM preview build (tools/wasm/build.sh) the
// EPD/font headers transitively pull ESP-IDF driver headers that the
// host toolchain can't compile. Gate them off; the pure-logic decls
// below (FormatDigits, PriceInt, SatsPerUnit, ComputeMoscowLayout,
// CurrencySymbolUtf8, DigitLayout) are independent of them, and PrepFb
// isn't called from the WASM build.
#ifndef BTCLOCK_WASM_BUILD
#include "epd_ssd1680.hpp"
#include "font.hpp"
#endif

#include "screens/screen_math.hpp"

namespace btclock {

// Every digit call site in the screen renderers passes this. Don't widen
// it with punctuation or anything else with a deep descender: the ref
// box's `below_baseline` directly lowers the computed glyph baseline,
// and mixing wider and narrower refs across screens produces visibly
// inconsistent vertical positions (see test_host/test_screen_ref_chars
// for the regression that motivated pinning this).
inline constexpr const char* kDigitRef = "0123456789";

#ifndef BTCLOCK_WASM_BUILD
// Build a LandscapeFb view over panel `i`'s framebuffer. Templated on N
// so the array-of-arrays type propagates naturally; there's no allocation.
template <size_t N>
LandscapeFb PrepFb(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                   uint8_t (&fb_storage)[N][16 * 296], size_t i) {
  LandscapeFb lfb = {};
  lfb.native_fb = fb_storage[i];
  lfb.native_stride = panels[i]->kStride;
  lfb.native_width = panels[i]->Width();
  lfb.native_height = panels[i]->Height();
  lfb.rotation = Rotation::k180;
  return lfb;
}
#endif

// Right-justify the decimal form of `h` into `digits[slots]`; leading
// positions get ' ' as blanks. Leading digits are truncated if `h`
// exceeds `slots` — the next block-height decade rollover is years out.
void FormatDigits(uint32_t h, char* digits, size_t slots);

// 1e8 / price_usd, rounded, clamped to [0, 4e9). Returns -1 on parse
// failure or out-of-range.
int32_t SatsPerUnit(const std::string& price_str);

// Integer part of price, rounded half-up. Returns -1 on parse failure
// or if the value exceeds the 6-digit display range (> 2e9).
int32_t PriceInt(const std::string& price_str);

struct DigitLayout {
  std::array<char, 6> digits{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sats{};
};

// Lay out up to 6 digits from `sats` with an optional sats-glyph prefix
// placed one slot before the first digit. Returns all-blank on `sats<0`.
// On overflow (> 6 digits), leading digits are truncated and no symbol.
DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol);

// UTF-8 currency symbol for the given ISO code, or "" if no glyph is
// available yet (the Antonio subset covers $, £, ¥, €; see
// components/fonts/assets/README.md for the codepoint list). Empty
// means the price screen won't paint a symbol panel — just the label
// and digits. Callers treat empty the same as "no symbol requested".
const char* CurrencySymbolUtf8(const std::string& ccy);

// Pure halving / supply / clock math moved to screen_math.hpp so host
// tests can include it without pulling the ESP-IDF-dependent font and
// EPD headers above. See screen_math.hpp for the helpers themselves.

}  // namespace btclock
