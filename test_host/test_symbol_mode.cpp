// Host tests for ResolveSymbolMode. The WebUI gates the ₿ toggle on
// the active font's hasBtcSymbol catalog flag, but a direct PATCH or
// a font swap can leave use_btc_symbol=true on a font that lacks
// U+20BF; ScreenManager::Render runs this resolver every frame so
// the marker collapses to "no marker" instead of painting tofu.

#include "doctest.h"
#include "screens/symbol_mode.hpp"

using btclock::ResolveSymbolMode;
using btclock::SymbolModeOutcome;

TEST_CASE("symbol_mode: no markers requested → both flags stay off") {
  const auto out = ResolveSymbolMode(/*sats=*/false, /*btc=*/false,
                                     /*has_btc=*/true);
  CHECK_FALSE(out.use_sats_symbol);
  CHECK_FALSE(out.use_btc_symbol);
}

TEST_CASE("symbol_mode: sats requested → sats stays on regardless of btc font") {
  // The sats glyph rides a dedicated PUA font role (kSatsGlyph) that
  // is independent of the digit-font's U+20BF coverage; the resolver
  // must not touch use_sats_symbol when has_btc=false.
  for (bool has_btc : {false, true}) {
    CAPTURE(has_btc);
    const auto out = ResolveSymbolMode(/*sats=*/true, /*btc=*/false, has_btc);
    CHECK(out.use_sats_symbol);
    CHECK_FALSE(out.use_btc_symbol);
  }
}

TEST_CASE("symbol_mode: ₿ requested + font has U+20BF → ₿ stays on") {
  const auto out = ResolveSymbolMode(/*sats=*/false, /*btc=*/true,
                                     /*has_btc=*/true);
  CHECK_FALSE(out.use_sats_symbol);
  CHECK(out.use_btc_symbol);
}

TEST_CASE("symbol_mode: ₿ requested + font lacks U+20BF → ₿ downgraded off") {
  const auto out = ResolveSymbolMode(/*sats=*/false, /*btc=*/true,
                                     /*has_btc=*/false);
  CHECK_FALSE(out.use_sats_symbol);
  CHECK_FALSE(out.use_btc_symbol);
}

TEST_CASE("symbol_mode: ₿ downgrade does NOT promote to sats") {
  // Deliberate: the user picked ₿ explicitly. Falling back to a
  // different glyph (sats) would be a quieter form of the same
  // surprise. Marker cell is left empty; layout collapses cleanly.
  const auto out = ResolveSymbolMode(/*sats=*/false, /*btc=*/true,
                                     /*has_btc=*/false);
  CHECK_FALSE(out.use_sats_symbol);
}

TEST_CASE("symbol_mode: simultaneous sats+btc on a btc-capable font") {
  // priceSymMode is mutually exclusive at the settings layer (single
  // u8), so this combination doesn't arise from the WebUI; verify
  // the resolver still behaves cleanly if some other caller passes
  // both true. The resolver does not arbitrate between them — the
  // mutually-exclusive contract is enforced upstream.
  const auto out = ResolveSymbolMode(/*sats=*/true, /*btc=*/true,
                                     /*has_btc=*/true);
  CHECK(out.use_sats_symbol);
  CHECK(out.use_btc_symbol);
}

TEST_CASE("symbol_mode: simultaneous sats+btc downgraded if font lacks ₿") {
  // Same case as above, but the font cannot draw U+20BF; the resolver
  // strips ₿ and lets the upstream sats request through unchanged.
  const auto out = ResolveSymbolMode(/*sats=*/true, /*btc=*/true,
                                     /*has_btc=*/false);
  CHECK(out.use_sats_symbol);
  CHECK_FALSE(out.use_btc_symbol);
}
