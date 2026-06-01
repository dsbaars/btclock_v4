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
//      first digit (use_symbol gates the glyph). When the integer is
//      wider than the digit region — e.g. sats-per-XAU 6,261,741 (7
//      digits) on a 7-panel board's 6 digit slots — it switches to the
//      K/M/B/T suffix form ("6.26M") instead of dropping the leading
//      digit (which silently misreported the value as "261741"). The
//      8-panel board fits 7-digit sats outright, so it still shows every
//      digit; the suffix only kicks in where the digits genuinely don't
//      fit. Mirrors the BTC-price screen's overflow-to-suffix path.
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
//
// `share_dot` (mirrors the global decimalShareDot pref): when true on
// the fractional path, fold the '.' into the cell holding the leading
// '0' so "0.0392" renders as ["0.","0","3","9","2"] across one fewer
// cell and the freed slot can carry an extra fractional digit. Same
// layout convention LayoutBtcPriceSuffixStrings uses on the BTC price
// screen, so a user setting decimalShareDot=true gets a consistent
// look across every decimal layout.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "screens/screen_math.hpp"  // FormatNumberWithSuffix

namespace btclock {

template <std::size_t Slots>
struct SatsPerCurrencyLayout {
  // Per-cell text. ' ' (single space) and "" both mark a blank cell;
  // single ASCII digit ('0'..'9') for digit cells; "." for a standalone
  // decimal-point cell on the share_dot=false fractional path; "0." for
  // the merged leading cell on the share_dot=true fractional path. The
  // string form (rather than a single char) is what lets share_dot
  // collapse two cells into one without bending the rest of the API.
  std::array<std::string, Slots> cells{};
  // Cell carries the sats-glyph marker. Only set on the >=1 integer
  // path with use_symbol=true; the fractional path leaves this all-false.
  std::array<bool, Slots> is_sats{};
  // True when the layout used the fractional ("0.dddd") path. Caller
  // uses this to suppress the "MSCW/TIME" label gate (Moscow-time only
  // makes sense for the >=1 integer regime).
  bool fractional = false;
};

// Maximum sats-per-currency value the integer path will accept. Anything
// above this is treated as a parse glitch / data-feed bug and renders
// as all-blank rather than wrapping. Mirrors the int32 clamp the older
// SatsPerUnit() helper applied.
inline constexpr double kSatsPerCurrencyMax = 4e9;

template <std::size_t Slots>
inline SatsPerCurrencyLayout<Slots> ComputeSatsPerCurrencyLayout(
    const std::string& price_str, bool use_symbol, bool share_dot) {
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
    // share_dot has no effect here (no '.' to fold).
    const std::int64_t sats = static_cast<std::int64_t>(sats_d + 0.5);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(sats));
    const std::size_t len = std::strlen(buf);
    if (len > Slots) {
      // Overflow — the integer is wider than the digit region. Dropping
      // the leading digit (the old behaviour) silently changed the value:
      // sats-per-XAU 6,261,741 became "261741" on a 7-panel board. Render
      // the K/M/B/T suffix form instead, mirroring the BTC-price screen's
      // overflow path. The 8-panel board fits 7-digit sats outright via
      // the len<=Slots path below, so this only fires on the narrower
      // 7-panel boards (or for 8+-digit sats on any board). The sats glyph
      // sits one slot before the suffix number when use_symbol and there's
      // room — the suffix's compression frees the cell the raw integer
      // could not spare.
      int budget = static_cast<int>(use_symbol ? Slots - 1 : Slots);
      std::string suffix =
          FormatNumberWithSuffix(static_cast<std::uint64_t>(sats), budget);
      bool glyph = use_symbol;
      // Defensive: the K..Q ladder always fits `budget >= 2`, but if the
      // glyph can't sit beside the suffix string, drop it and re-pack at
      // full width rather than overflow the cell array.
      if (glyph && suffix.size() + 1 > Slots) {
        glyph = false;
        suffix = FormatNumberWithSuffix(static_cast<std::uint64_t>(sats),
                                        static_cast<int>(Slots));
      }
      const std::size_t width = suffix.size() + (glyph ? 1u : 0u);
      std::size_t i = width <= Slots ? Slots - width : 0;  // right-justify
      if (glyph && i < Slots) {
        l.is_sats[i++] = true;
      }
      for (std::size_t j = 0; j < suffix.size() && i < Slots; ++j) {
        l.cells[i++].assign(1, suffix[j]);
      }
      return l;
    }
    // Fits (exactly or with room) — right-justify across the digit cells.
    // At an exact-width fit (pad == 0, e.g. 7-digit sats on the 8-panel
    // board) every cell holds a digit and the glyph is simply dropped.
    const std::size_t pad = Slots - len;
    for (std::size_t i = 0; i < Slots; ++i) {
      if (i >= pad) l.cells[i].assign(1, buf[i - pad]);
    }
    if (use_symbol && pad > 0) {
      l.is_sats[pad - 1] = true;
    }
    return l;
  }

  // Fractional path — "0.dddd".
  //
  // share_dot=false (default): "0", ".", and the fractional digits each
  // get their own cell.
  //
  // share_dot=true: the '.' folds into the leading cell as "0.", freeing
  // one slot for an extra fractional digit. Mirrors the BTC-price suffix
  // layout's shareDot branch so a user setting decimalShareDot=true gets
  // a consistent look across every decimal layout.
  //
  // use_symbol=true reserves one leading cell for the sats glyph (cells[0]
  // stays empty, is_sats[0] is set) so the screen still identifies the
  // unit even when sats-per-currency drops below 1. The cost is one
  // fractional digit; users who want maximum decimal precision can pair
  // it with decimalShareDot=true to win one digit back.
  //
  // The format string fills any unused trailing slots with zeros (e.g.
  // exactly 2 sats per 1e8 → "0.5000" on a 6-slot board) — that's a
  // deliberate choice over rendering "0.5   " so the precision is
  // unambiguous.
  l.fractional = true;
  // Layout budget — start with the cells the leading "0[.]" + optional
  // '.' + optional sats-glyph slot consumes; whatever's left is the
  // fractional-digit budget. Fall back to right-justified "0" if the
  // panel can't fit even the minimum.
  const std::size_t reserved = (share_dot ? 1u : 2u) + (use_symbol ? 1u : 0u);
  if (Slots <= reserved) {
    l.cells[Slots - 1] = "0";
    return l;
  }
  const int frac_digits = static_cast<int>(Slots - reserved);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", frac_digits, sats_d);
  // Defensive: rounding right-up to "1.0000" (sats_d just under 1.0
  // rounding up) — fall back to integer cell '1' right-justified so
  // the layout doesn't silently drop the leading digit.
  if (buf[0] != '0') {
    l.fractional = false;
    l.cells[Slots - 1] = "1";
    if (use_symbol && Slots > 1) {
      l.is_sats[Slots - 2] = true;
    }
    return l;
  }
  // Walk `buf` (which is "0.<digits>") from left to right, mapping each
  // character to a cell. share_dot=true emits "0." into a single cell
  // at the same position the share_dot=false path would emit "0". When
  // use_symbol=true, cells[0] stays empty and is_sats[0] flags the
  // sats-glyph slot — same convention the integer path uses.
  std::size_t i = 0;  // cell index
  if (use_symbol) {
    l.is_sats[i++] = true;
  }
  if (share_dot) {
    l.cells[i++] = "0.";
    // Skip "0." in buf.
    std::size_t j = 2;
    while (j < std::strlen(buf) && i < Slots) {
      l.cells[i++].assign(1, buf[j++]);
    }
  } else {
    std::size_t j = 0;
    while (buf[j] != '\0' && i < Slots) {
      l.cells[i++].assign(1, buf[j++]);
    }
  }
  return l;
}

}  // namespace btclock
