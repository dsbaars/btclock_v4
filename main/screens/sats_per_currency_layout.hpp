// Pure-logic layout helper for the SATS/<CCY> (a.k.a. Moscow time)
// screen. Both the on-device renderer (moscow_time.cpp) and the WebUI
// panel-text mirror (panel_texts.cpp BuildMoscowTime) feed through here
// so the displayed cells agree byte-for-byte.
//
// Two regimes:
//
//   1. >= 1 sat per currency unit (the classic Moscow-time case for
//      USD, and the typical case for every other currency the v2 feed
//      currently lists). Layout: integer sats right-justified across
//      `Slots` cells with an optional sats glyph one slot before the
//      first digit (use_symbol gates the glyph).
//
//   2. < 1 sat per currency unit. Reachable for runtime-fetched codes
//      from `/api/v2/currencies` whose 1-unit value exceeds 1 BTC's
//      sat-equivalent (e.g. weak fiat like VND, IRR, LBP). Returning
//      the rounded integer ("0") for these is a regression — users
//      lose the precision they care about. Layout: "0.dddd" filling
//      every slot with as many fractional digits as fit, no sats glyph.
//
// The decimal path is the one this header was extracted to make
// host-testable; the integer path is a straight port of the older
// ComputeMoscowLayoutN<Slots> in common.hpp (kept there for the legacy
// callers that still pass int32 sats — primarily the WASM preview
// build).

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace btclock {

template <std::size_t Slots>
struct SatsPerCurrencyLayout {
  // Per-cell character. ' ' marks blank/padded cells; '0'..'9' for
  // digits; '.' for the decimal point in the fractional path.
  std::array<char, Slots> digits{};
  // Cell carries the sats-glyph marker. Only set on the >=1 integer
  // path with use_symbol=true; the fractional path leaves this all-false.
  std::array<bool, Slots> is_sats{};
  // True when the layout used the fractional ("0.dddd") path. Caller
  // uses this to suppress the "MSCW/TIME" label gate (Moscow-time only
  // makes sense for the >=1 integer regime).
  bool fractional = false;

  SatsPerCurrencyLayout() {
    for (std::size_t i = 0; i < Slots; ++i) {
      digits[i] = ' ';
      is_sats[i] = false;
    }
  }
};

// Maximum sats-per-currency value the integer path will accept. Anything
// above this is treated as a parse glitch / data-feed bug and renders
// as all-blank rather than wrapping. Mirrors the int32 clamp the older
// SatsPerUnit() helper applied.
inline constexpr double kSatsPerCurrencyMax = 4e9;

template <std::size_t Slots>
inline SatsPerCurrencyLayout<Slots> ComputeSatsPerCurrencyLayout(
    const std::string& price_str, bool use_symbol) {
  SatsPerCurrencyLayout<Slots> l;
  if (Slots == 0 || price_str.empty()) return l;

  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (endp == price_str.c_str()) return l;
  if (!(p > 0.0)) return l;
  const double sats_d = 1e8 / p;
  if (!(sats_d > 0.0)) return l;
  if (sats_d > kSatsPerCurrencyMax) return l;

  if (sats_d >= 1.0) {
    // Integer path — half-up rounding to match the older int32 helper.
    const std::int64_t sats = static_cast<std::int64_t>(sats_d + 0.5);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(sats));
    const std::size_t len = std::strlen(buf);
    if (len >= Slots) {
      const std::size_t start = len - Slots;
      for (std::size_t i = 0; i < Slots; ++i) l.digits[i] = buf[start + i];
      return l;
    }
    const std::size_t pad = Slots - len;
    for (std::size_t i = 0; i < Slots; ++i) {
      l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
    }
    if (use_symbol && pad > 0) {
      l.is_sats[pad - 1] = true;
      l.digits[pad - 1] = ' ';
    }
    return l;
  }

  // Fractional path — "0.dddd". Reserve 2 cells for the leading "0."
  // and use every remaining cell for fractional precision. The format
  // string fills any unused trailing slots with zeros (e.g. exactly 2
  // sats per 1e8 → "0.5000" on a 6-slot board) — that's a deliberate
  // choice over rendering "0.5   " so the precision is unambiguous.
  l.fractional = true;
  if (Slots < 3) {
    // Degenerate budget — fall back to "0" right-justified.
    l.digits[Slots - 1] = '0';
    return l;
  }
  const int frac_digits = static_cast<int>(Slots) - 2;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", frac_digits, sats_d);
  std::size_t len = std::strlen(buf);
  // %.*f on an input < 1 always emits "0.<digits>" — len == frac_digits+2 ==
  // Slots. Defensive: if rounding produced "1.0000" (sats_d just under 1.0
  // rounding up), fall back to integer cell '1' right-justified.
  if (buf[0] != '0') {
    l.fractional = false;
    for (std::size_t i = 0; i < Slots; ++i) l.digits[i] = ' ';
    l.digits[Slots - 1] = '1';
    if (use_symbol && Slots > 1) {
      l.is_sats[Slots - 2] = true;
    }
    return l;
  }
  if (len > Slots) {
    // Should not happen for sats_d < 1 with frac_digits = Slots - 2,
    // but truncate trailing characters defensively.
    len = Slots;
  }
  const std::size_t pad = Slots - len;
  for (std::size_t i = 0; i < Slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  return l;
}

}  // namespace btclock
