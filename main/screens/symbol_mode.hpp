// Pure helper resolving the effective price-marker symbol mode from
// the user prefs and the active digit font's glyph coverage. The
// WebUI hides the ₿ option for fonts whose digit subset omits U+20BF
// (the available-fonts catalog ships hasBtcSymbol per font), but
// nothing prevents a direct `PATCH /api/settings priceSymMode=2`
// request, and a font swap after the toggle leaves the
// already-stored pref intact. Without a server-side downgrade,
// stb_truetype would draw a blank box where the marker should be.
//
// Header-only (no IDF / NVS / Font dependency) so test_host can pin
// the truth table.

#pragma once

namespace btclock {

struct SymbolModeOutcome {
  bool use_sats_symbol;
  bool use_btc_symbol;
};

inline SymbolModeOutcome ResolveSymbolMode(bool requested_sats_symbol,
                                           bool requested_btc_symbol,
                                           bool digit_font_has_btc_sign) {
  SymbolModeOutcome out{requested_sats_symbol, requested_btc_symbol};
  if (out.use_btc_symbol && !digit_font_has_btc_sign) {
    // No glyph at U+20BF — collapse the marker rather than paint tofu.
    // We deliberately do NOT promote to use_sats_symbol: the user
    // chose ₿ explicitly; falling back to a different glyph would be
    // a quieter form of the same surprise. The marker cell is left
    // empty; the layout collapses cleanly.
    out.use_btc_symbol = false;
  }
  return out;
}

}  // namespace btclock
