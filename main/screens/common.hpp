// Private helpers shared across screen renderers.
//
// Not part of the public screens API — only the per-screen .cpp files
// include this. Keep it header-only where it matters (tiny templates /
// constants) and push non-template impls into common.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Under the emscripten/WASM preview build (tools/wasm/build.sh) the
// real epd_ssd1680.hpp can't be pulled in — it pulls driver/gpio.h,
// driver/spi_master.h, mcp23017.hpp. We substitute a minimal EpdPanel
// shim (wasm_panel.hpp) that exposes just the Width/Height/kStride
// surface and stubs out DrawFramebufferStart/WaitForRefresh. font.hpp
// itself is pure (cstdint/cstddef only) so it's the same in both
// builds — it carries LandscapeFb/Rotation + the paint-primitive decls
// that every renderer uses.
#ifdef BTCLOCK_WASM_BUILD
#include "wasm_panel.hpp"
#include "font.hpp"
#else
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

// Build a LandscapeFb view over panel `i`'s framebuffer. Templated on N
// so the array-of-arrays type propagates naturally; there's no allocation.
// Compiles against either the real EpdPanel (components/epd_ssd1680) or
// the WASM shim (tools/wasm/wasm_panel.hpp) — both expose the same
// kStride/Width/Height surface.
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
// Divergence vs. old firmware: old parseSatsPerCurrency drops the
// SATS/MSCW label on 7-digit sats (price < ~$100) and fills all 7
// panels. Not fixed here — tracked in btclock_v3_fci-f7y.
DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol);

// Templated variant used by the V8 8-panel layout (Bug 3). The 6-slot
// fixed `ComputeMoscowLayout` left panel 7 blank; this helper adapts to
// an arbitrary digit-slot count so the renderer can fill all N-1 slots.
// Returns all-blank on `sats<0`. On overflow (> Slots digits), leading
// digits are truncated and no sats symbol is emitted.
template <size_t Slots>
struct MoscowLayoutN {
  std::array<char, Slots> digits{};
  std::array<bool, Slots> is_sats{};
  MoscowLayoutN() {
    for (size_t i = 0; i < Slots; ++i) {
      digits[i] = ' ';
      is_sats[i] = false;
    }
  }
};

template <size_t Slots>
inline MoscowLayoutN<Slots> ComputeMoscowLayoutN(int32_t sats,
                                                 bool use_symbol) {
  MoscowLayoutN<Slots> l;
  if (sats < 0) return l;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
  const size_t len = std::strlen(buf);
  if (len >= Slots) {
    const size_t start = len - Slots;
    for (size_t i = 0; i < Slots; ++i) l.digits[i] = buf[start + i];
    return l;
  }
  const size_t pad = Slots - len;
  for (size_t i = 0; i < Slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  if (use_symbol && pad > 0) {
    l.is_sats[pad - 1] = true;
    l.digits[pad - 1] = ' ';
  }
  return l;
}

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
