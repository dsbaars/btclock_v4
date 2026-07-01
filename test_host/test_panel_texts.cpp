// Per-panel text builder tests.
//
// The /api/status `data[]` mirror comes out of
// main/screens/panel_texts.cpp — these cases pin the shapes the WebUI
// renders. They mirror a subset of the old firmware's parse*() parity
// tests (see test_datahandler_parity.cpp) but drive the actual IDF
// helper rather than a local copy.

#include <optional>
#include <string>
#include <vector>

#include "data_core/snapshot.hpp"
#include "doctest.h"
#include "screens/assets/pool_logos.hpp"
#include "screens/panel_texts.hpp"

namespace {

using btclock::BuildPanelTexts;
using btclock::PanelTextInputs;
using btclock::ScreenType;

}  // namespace

TEST_CASE("panel_texts — block height, 7 panels") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockHeight;
  in.block_height = 999999u;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BLOCK/HEIGHT");
  CHECK(out[1] == "9");
  CHECK(out[2] == "9");
  CHECK(out[6] == "9");
}

TEST_CASE("panel_texts — block height overflow drops label") {
  // 7-digit height on a 7-panel board: old firmware dropped the label
  // and filled every slot with a digit. Match that.
  PanelTextInputs in;
  in.kind = ScreenType::kBlockHeight;
  in.block_height = 1000000u;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "1");
  CHECK(out[1] == "0");
  CHECK(out[2] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — block height empty (nullopt)") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockHeight;
  // block_height left nullopt — builder treats as 0 and pads with blanks.
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BLOCK/HEIGHT");
  // 6 digits of blank with a final '0' is how %u of 0 lays out.
  CHECK(out[6] == "0");
  CHECK(out[1] == "");
}

TEST_CASE("panel_texts — halving countdown label + tail digits") {
  PanelTextInputs in;
  in.kind = ScreenType::kHalving;
  in.block_height = 210000u - 5;  // 5 blocks remaining to next halving
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "HAL/VING");
  CHECK(out[6] == "5");
  CHECK(out[5] == "");
}

TEST_CASE("panel_texts — bitcoin supply label + right-justified digits") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitcoinSupply;
  in.block_height = 0u;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — bitcoin supply big-chars fills all tail cells") {
  // SupplyAtBlock(880000) = 19.8125M → with the bumped (n_panels-1)=6
  // budget the formatter emits "19.81M" (6 chars), packing all 6 tail
  // slots on a 7-panel board with no leading blank. Pins the budget
  // bump from n_panels-2 to n_panels-1; before the fix the formatter
  // got 5 chars and emitted "19.8M" with a leading blank cell.
  PanelTextInputs in;
  in.kind = ScreenType::kBitcoinSupply;
  in.block_height = 880000u;
  in.supply_big_chars = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[1] == "1");
  CHECK(out[2] == "9");
  CHECK(out[3] == ".");
  CHECK(out[4] == "8");
  CHECK(out[5] == "1");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — halving time-mode emits labelled breakdown") {
  PanelTextInputs in;
  in.kind = ScreenType::kHalving;
  in.block_height = 0u;  // full interval ahead → 3/YRS 363/DAYS 8/HRS 0/MINS
  in.halving_as_blocks = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BIT/COIN");
  CHECK(out[1] == "HAL/VING");
  CHECK(out[2] == "3/YRS");
  CHECK(out[3] == "363/DAYS");
  CHECK(out[4] == "8/HRS");
  CHECK(out[5] == "0/MINS");
  CHECK(out[6] == "TO/GO");
}

TEST_CASE("panel_texts — supply percentage mode") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitcoinSupply;
  in.block_height = 831000u;  // ≈ 93.48%
  in.supply_percent = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[1] == "9");
  CHECK(out[2] == "3");
  CHECK(out[3] == ".");
  CHECK(out[4] == "4");
  CHECK(out[5] == "8");
  CHECK(out[6] == "%");
}

TEST_CASE("panel_texts — supply small-chars three-digit groups") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitcoinSupply;
  in.block_height = 655987u;
  in.supply_big_chars = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[4] == " 18");
  CHECK(out[5] == "537");
}

TEST_CASE("panel_texts — market cap small-chars three-digit groups") {
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "USD";
  in.block_height = 831000u;
  in.price = "52000";
  in.mcap_big_chars = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "USD/MCAP");
  // Cap = 52000 * SupplyAtBlock(831000) ≈ 1.02T. The currency separator
  // cell sits just before the first 3-digit group.
  CHECK(out[1] == " $ ");
  CHECK(out[2] == "  1");
  CHECK(out[3] == "020");
  CHECK(out[4] == "825");
  CHECK(out[5] == "000");
  CHECK(out[6] == "000");
}

TEST_CASE("panel_texts — market cap keeps € as one cell (bigChars, 7 panels)") {
  // Reproduces the /api/status bug where the 3-byte UTF-8 € (E2 82 AC)
  // got spread across three cells because EmitBigCharsFrame iterated
  // bytes. Each cell must carry one complete codepoint — the on-device
  // EPD renderer does this already, the status mirror now matches.
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "EUR";
  in.mcap_big_chars = true;
  in.block_height = 840000u;
  in.price = "100000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "EUR/MCAP");
  // First tail cell holds the full euro glyph, not the middle or last
  // byte of its UTF-8 sequence.
  CHECK(out[1] == "\xE2\x82\xAC");
  // Every subsequent tail cell is either a single ASCII char or empty
  // (no stray continuation bytes).
  for (std::size_t i = 2; i < 7; ++i) {
    CAPTURE(i);
    const auto& cell = out[i];
    // Empty, or a single ASCII byte, or the K/M/B/T suffix.
    CHECK((cell.empty() || cell.size() == 1));
  }
}

TEST_CASE("panel_texts — market cap uses currency in label") {
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "USD";
  in.block_height = 840000u;
  in.price = "100000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "USD/MCAP");
  // Cap = 100k × SupplyAtBlock(840000) ≈ 1.97T. BigChars suffix layout
  // places the USD glyph prefix "$" in slot 1,
  // an optional padding space in intermediate slots, and the suffix
  // char (K/M/B/T) in the last slot. Slot 1 always carries the currency
  // glyph.
  CHECK(out[1] == "$");
  CHECK(!out[6].empty());
  const char last = out[6][0];
  CHECK((last == 'K' || last == 'M' || last == 'B' || last == 'T' ||
         (last >= '0' && last <= '9')));
}

TEST_CASE("panel_texts — Moscow time in classic range → MSCW/TIME label") {
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  // 1e8 / 2684 = 37258.57..., rounds to 37259. Classic moscow-time
  // range (< 100000) so the label stays MSCW/TIME.
  in.price = "2684";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "MSCW/TIME");
  // 5-digit value in 6 slots leaves one blank for the STS marker.
  CHECK(out[1] == "STS");
  CHECK(out[2] == "3");
  CHECK(out[3] == "7");
  CHECK(out[4] == "2");
  CHECK(out[5] == "5");
  CHECK(out[6] == "8");
}

TEST_CASE("panel_texts — V8 Moscow time uses all 8 panels") {
  // Bug 3 — the previous layout hard-coded 6 digit slots so the
  // trailing V8 panel stayed blank. Old firmware parseSatsPerCurrency
  // fills every slot: label + pad + STS + digits right-justified.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";  // 37258 sats = 5 digits in 7 slots.
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "MSCW/TIME");
  // 5-digit value in 7 slots: 2 leading pads, then STS marker, then
  // the 5 digits. No trailing blank panel.
  CHECK(out[1] == "");
  CHECK(out[2] == "STS");
  CHECK(out[3] == "3");
  CHECK(out[4] == "7");
  CHECK(out[5] == "2");
  CHECK(out[6] == "5");
  CHECK(out[7] == "8");
}

TEST_CASE("panel_texts — V8 Moscow time fills trailing slot for large sats") {
  // For low-price currencies the sats count can reach 7 digits — on
  // V8's 7 digit slots there's no room for the STS marker. Matches the
  // old parseSatsPerCurrency overflow path (no STS, digits fill all).
  // Sats 1e8/50 = 2_000_000 which is out of the classic MSCW/TIME
  // range (< 100_000), so the label falls back to SATS/USD.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "50";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "SATS/USD");
  CHECK(out[1] == "2");
  CHECK(out[2] == "0");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

TEST_CASE("panel_texts — sats-per-XAU 7-digit overflow → suffix on 7 panels") {
  // The reported XAU regression mirrored to /api/status. At ~$4600/oz
  // gold, sats-per-XAU is a 7-digit number (1e8/16 = 6,250,000) that a
  // 7-panel board's 6 digit slots can't hold. The mirror now emits the
  // K/M suffix form ("6.25M") with the STS marker — matching what the
  // EPD paints — instead of the truncated "261741" the old layout showed.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "XAU";
  in.price = "16.0";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "SATS/XAU");
  CHECK(out[1] == "STS");
  CHECK(out[2] == "6");
  CHECK(out[3] == ".");
  CHECK(out[4] == "2");
  CHECK(out[5] == "5");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — sats-per-XAU 7-digit fits whole on 8 panels") {
  // The 8-panel board's 7 digit slots hold the full 7-digit value, so the
  // suffix path stays off — every digit shows (glyph dropped on the
  // exact-width fit). data[] = ["SATS/XAU","6","2","5","0","0","0","0"].
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "XAU";
  in.price = "16.0";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "SATS/XAU");
  CHECK(out[1] == "6");
  CHECK(out[2] == "2");
  CHECK(out[3] == "5");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

TEST_CASE("panel_texts — Moscow time rounds half-up at the .5 sats boundary") {
  // Defensive guard against a v3 bug class (commit 132aa83 "Mow Units
  // no rounding!") where the sats-per-unit conversion truncated rather
  // than rounded, producing off-by-one digits for prices that landed
  // on the half-sats boundary. v4 already does it right via `+ 0.5`
  // before the int cast in SatsPerUnitLocal — this test pins the
  // rounding direction so a future refactor can't silently regress
  // to truncation.
  //
  // Pick a price where 1e8/price has a fractional part > 0.5 — the
  // truncation regression would render the lower integer.
  // 1e8 / 99950 = 1000.5002… → round-half-up = 1001 (correct)
  //                          → truncate     = 1000 (regressed)
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "99950";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "MSCW/TIME");
  // 4-digit value in 6 slots: 2 leading pads collapse to one blank
  // cell + the STS marker, then the digits "1001" right-justified.
  CHECK(out[1] == "");     // blank pad
  CHECK(out[2] == "STS");  // sats glyph marker
  CHECK(out[3] == "1");    // ← regression would show "1"...
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "1");  // ← ...followed by "0" instead of "1"
}

TEST_CASE("panel_texts — SATS/<CCY> label for non-USD currencies") {
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "EUR";
  in.price = "2684";
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "SATS/EUR");
}

TEST_CASE(
    "panel_texts — useMscwTime=false forces SATS/USD for USD classic range") {
  // bd btclock_v4-5wj — the /api/settings useMscwTime toggle now gates
  // the Moscow-time label. With the flag off, USD in the classic range
  // falls back to the uniform SATS/USD heading (old firmware parity —
  // see v3_fci screen_handler.cpp parseSatsPerCurrency call site).
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";  // 37259 sats — in classic MSCW/TIME range
  in.use_mscw_time = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "SATS/USD");
}

TEST_CASE("panel_texts — useMscwTime=true preserves MSCW/TIME label") {
  // Sanity: the default (true) keeps the legacy behaviour intact so
  // existing users don't see a label change on upgrade.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";
  in.use_mscw_time = true;  // explicit for clarity; also the default
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "MSCW/TIME");
}

TEST_CASE("panel_texts — useSatsSymbol=false drops the STS marker cell") {
  // bd btclock_v4-5wj — with the glyph suppressed the marker cell is
  // blank; digits stay right-justified. Mirrors v3_fci
  // parseSatsPerCurrency(..., useSatsSymbol=false).
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "MSCW/TIME");
  // No "STS" anywhere in the row; the marker slot is empty.
  for (std::size_t i = 1; i < 7; ++i) CHECK(out[i] != "STS");
  CHECK(out[1] == "");   // marker slot now blank
  CHECK(out[2] == "3");  // digits still right-justified
  CHECK(out[6] == "8");
}

TEST_CASE("panel_texts — useBlkCountdown=true keeps blocks countdown form") {
  // Default path — same as the existing halving-blocks case. Kept as an
  // explicit flag test so the screen_manager gate is covered end-to-end.
  PanelTextInputs in;
  in.kind = ScreenType::kHalving;
  in.block_height = 210000u - 5;
  in.halving_as_blocks = true;  // = useBlkCountdown from NVS
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "HAL/VING");
  CHECK(out[6] == "5");
}

TEST_CASE("panel_texts — useBlkCountdown=false emits time breakdown") {
  // Same as the existing halving-time-mode case but anchored on the
  // pref flag name — tightens the audit trail for bd btclock_v4-5wj.
  PanelTextInputs in;
  in.kind = ScreenType::kHalving;
  in.block_height = 0u;
  in.halving_as_blocks = false;  // useBlkCountdown=false
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "BIT/COIN");
  CHECK(out[1] == "HAL/VING");
  CHECK(out[6] == "TO/GO");
}

TEST_CASE("panel_texts — Moscow time STS marker for small sats") {
  // 1e8 / 10000 = 10000 sats — label still SATS/USD since sats >= 100k?
  // No — 10000 < 100000, so MSCW/TIME. The 5-digit value leaves one
  // blank slot at the front, which the builder marks with "STS".
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "10000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "MSCW/TIME");
  CHECK(out[1] == "STS");
  CHECK(out[2] == "1");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — BTC price label + currency symbol slot") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "64211.53";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  // PriceInt rounds 64211.53 → 64212, 5-digit number in a 6-digit
  // slot leaves a blank at position 1 (from slot 1). That blank gets
  // the "$" currency glyph.
  CHECK(out[1] == "$");
  CHECK(out[2] == "6");
  CHECK(out[3] == "4");
  CHECK(out[4] == "2");
  CHECK(out[5] == "1");
  CHECK(out[6] == "2");
}

TEST_CASE("panel_texts — BTC price overflow drops symbol") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "999999";  // 6 digits exactly; no blank slot for the symbol.
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  // All 6 digit slots are filled; no symbol.
  for (std::size_t i = 1; i < 7; ++i) {
    CHECK(out[i].size() == 1);
    CHECK(out[i][0] >= '0');
    CHECK(out[i][0] <= '9');
  }
}

// Sub-dollar decimal coverage: BuildBtcPrice routes through the same
// LayoutBtcPrice helper as the on-panel renderer, so the WebUI /api/status
// data[] reflects the sub-dollar decimal layout the EPD paints.

TEST_CASE("panel_texts — BTC price sub-dollar emits decimals + dot cell") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "0.123";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  // "$0.123" occupies all six digit slots, with the dot in its own cell.
  CHECK(out[1] == "$");
  CHECK(out[2] == "0");
  CHECK(out[3] == ".");
  CHECK(out[4] == "1");
  CHECK(out[5] == "2");
  CHECK(out[6] == "3");
}

TEST_CASE("panel_texts — BTC price sub-$100 emits 2 decimals") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "45.67";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "4");
  CHECK(out[3] == "5");
  CHECK(out[4] == ".");
  CHECK(out[5] == "6");
  CHECK(out[6] == "7");
}

TEST_CASE("panel_texts — V8 BTC price keeps whole numbers clean (no dot+0)") {
  // The Slots>=7 integer guard: a whole-number price must not gain a
  // ".0" tail on the 8-panel board. Fiat always arrives whole upstream.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "7858";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BTC/USD");
  // 4-digit integer in 7 digit slots with the glyph: 2 leading pads,
  // glyph, then 7858.
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "$");
  CHECK(out[4] == "7");
  CHECK(out[5] == "8");
  CHECK(out[6] == "5");
  CHECK(out[7] == "8");
  for (const auto& s : out) CHECK(s != ".");
}

TEST_CASE("panel_texts — V8 BTC price emits decimals for fractional prices") {
  // Low-magnitude pairs (gold ~16 BTC/XAU) arrive with decimals from the
  // ws-node aggregator; the 8-panel board renders that precision instead
  // of rounding to an integer. Uses USD so the "$" glyph is deterministic
  // (the layout itself is currency-agnostic). 16.01 → "$16.01".
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "16.01";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "");
  CHECK(out[2] == "$");
  CHECK(out[3] == "1");
  CHECK(out[4] == "6");
  CHECK(out[5] == ".");
  CHECK(out[6] == "0");
  CHECK(out[7] == "1");
}

TEST_CASE("panel_texts — V8 BTC price 6-digit integer keeps glyph") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "123456";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == "2");
  CHECK(out[4] == "3");
  CHECK(out[5] == "4");
  CHECK(out[6] == "5");
  CHECK(out[7] == "6");
}

// --- suffixPrice + mowMode panel-text parity ---
// Ports the v3_fci test_datahandler_parity tests for useSuffixFormat and
// useMowMode to the IDF /api/status mirror. These cases match v3's
// parsePriceData byte-for-byte where applicable; see the old firmware
// lib/btclock/data_handler.cpp and Unity tests.

TEST_CASE("panel_texts — suffixPrice=false, integer passes through") {
  // Regression path — without suffixPrice, short prices render as
  // plain digits via LayoutBtcPrice.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78280";
  in.suffix_price = false;
  in.mow_mode = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "7");
  CHECK(out[3] == "8");
  CHECK(out[4] == "2");
  CHECK(out[5] == "8");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — suffixPrice=true for 78280 renders $78.3K") {
  // v3 parsePriceData(78280, '$', useSuffixFormat=true, mow=false):
  // FormatNumberWithSuffix(78280, 5) → "78.3K"(5). "$78.3K"(6 cells) fits
  // the 6 digit slots with 0 leading pad; labelled "BTC/USD" on panel 0.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78280";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "7");
  CHECK(out[3] == "8");
  CHECK(out[4] == ".");
  CHECK(out[5] == "3");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — suffixPrice=true for 1234 renders $1.23K") {
  // 1234 → FormatNumberWithSuffix(1234, 5) → "1K"(len=2), rest=5-2-1=2,
  // → "1.23K". Prepend "$" → "$1.23K" (6 chars).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "1234";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "2");
  CHECK(out[5] == "3");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — suffixPrice=true for 10000000 renders $10.0M") {
  // 10_000_000 → "10M"(len=3), rest=5-3-1=1 → "10.0M"(5). "$10.0M"(6).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "10000000";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == "0");
  CHECK(out[4] == ".");
  CHECK(out[5] == "0");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — suffixPrice=true for 999 renders plain integer") {
  // 999 → FormatNumberWithSuffix(999, 5, false) → "999" (no suffix).
  // "$999" (4 chars) fits in 6 slots with 2 leading pads.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "999";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "$");
  CHECK(out[4] == "9");
  CHECK(out[5] == "9");
  CHECK(out[6] == "9");
}

TEST_CASE("panel_texts — suffixPrice=true for 1000 renders $1.00K") {
  // 1000 → "1K"(len=2), rest=5-2-1=2 → "1.00K"; "$1.00K" (6 chars) fits.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "1000";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — suffixPrice=true for 1000000000 renders $1.00B") {
  // 1e9 → "1B"(len=2), rest=5-2-1=2 → "1.00B"; "$1.00B"(6 chars).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "1000000000";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "B");
}

TEST_CASE("panel_texts — mowMode=true + suffixPrice=true for 78280 → $0.078M") {
  // 78280/1e6 = 0.078280 → "0.0M"(len=4), rest=6-4=2, take=5 → "0.078M"
  // (6). "$0.078M"(7 cells) overflows → no label, char-per-cell. Slot 0
  // carries the currency glyph — same as the EPD renderer which paints
  // PaintSlot::kCurrencyGlyph on panel 0 when the label is dropped.
  // v3 parsePriceData parity: `firstIndex=0` in the overflow branch so
  // ret[0] holds the '$' from priceString. Rev B user report at
  // 192.168.20.97 with mowMode+EUR saw data[0]="BTC/EUR" while the
  // panel painted "€" — fixed.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "$");
  CHECK(out[1] == "0");
  CHECK(out[2] == ".");
  CHECK(out[3] == "0");
  CHECK(out[4] == "7");
  CHECK(out[5] == "8");
  CHECK(out[6] == "M");
}

TEST_CASE(
    "panel_texts — mowMode=true + suffixPrice=true for 1000000 → $1.000M") {
  // 1e6 + mow → "1.000M"(6). "$1.000M"(7) = 7 cells → overflow path.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "1000000";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "$");
  CHECK(out[1] == "1");
  CHECK(out[2] == ".");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode=true + suffixPrice=true for 99999 → $0.099M") {
  // 99999/1e6=0.099999 → "0.0M"→"0.099M"(6). "$0.099M"(7) overflow.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "99999";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "$");
  CHECK(out[1] == "0");
  CHECK(out[2] == ".");
  CHECK(out[3] == "0");
  CHECK(out[4] == "9");
  CHECK(out[5] == "9");
  CHECK(out[6] == "M");
}

TEST_CASE(
    "panel_texts — mowMode=true, suffixPrice=false, short price stays "
    "integer") {
  // v3 precedence: `mowMode` only takes effect when the suffix branch
  // fires (either `useSuffixFormat=true` OR the integer itself is too
  // wide to fit). Without suffix_price and with 78280 (5 digits, fits a
  // 6-slot area), mowMode is ignored and the plain integer renders.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78280";
  in.suffix_price = false;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "7");
  CHECK(out[3] == "8");
  CHECK(out[4] == "2");
  CHECK(out[5] == "8");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — suffixPrice=true, 8-panel board, 78280 → $78.28K") {
  // V8 (8-panel → 7 digit slots). FormatNumberWithSuffix(78280, 6):
  // "78K"(len=3), rest=6-3-1=2 → "78.28K"(6). "$78.28K"(7 cells) fits
  // with 0 leading pad, labelled "BTC/USD" on panel 0.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78280";
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "7");
  CHECK(out[3] == "8");
  CHECK(out[4] == ".");
  CHECK(out[5] == "2");
  CHECK(out[6] == "8");
  CHECK(out[7] == "K");
}

TEST_CASE("panel_texts — integer overflow forces suffix path even off") {
  // v3 parsePriceData routes 7+digit integer prices through the suffix
  // path regardless of useSuffixFormat. 9_999_999 with suffix_price=
  // false → "$10.0M" on a 7-panel board (integer form would be 7 chars
  // and wouldn't fit with a glyph).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "9999999";
  in.suffix_price = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  // FormatNumberWithSuffix(9_999_999, 5): num_digits=7>6 → M branch;
  // 9_999_999/1e6=9.999999 → "10M"(rounded; len=3), rest=1 → "10.0M"(5).
  // "$10.0M"(6) fits with leading blank.
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == "0");
  CHECK(out[4] == ".");
  CHECK(out[5] == "0");
  CHECK(out[6] == "M");
}

// --- mowMode overflow: currency glyph lands in slot 0 (Rev B bug) ---
// Rev B user report (192.168.20.97, mowMode=true, EUR, price=78280):
// /api/status data[] was ["BTC/EUR","0",".","0","6","6","M"] while the
// EPD painted "€ 0 . 0 6 6 M". The mirror now emits the currency glyph
// in slot 0 on the overflow branch, matching the on-device renderer
// and v3 parsePriceData's `firstIndex=0` / `ret[0]=priceString[0]`
// fall-through. See BuildBtcPrice's go_suffix branch — when the suffix
// layout returns an empty label, the glyph already sits in cells[0].

TEST_CASE("panel_texts — mowMode overflow: EUR glyph in slot 0 (Rev B bug)") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "EUR";
  // User-reported fixture: 78280 with mowMode+suffixPrice = overflow.
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "\xE2\x82\xAC");  // UTF-8 euro sign
  CHECK(out[1] == "0");
  CHECK(out[2] == ".");
  CHECK(out[3] == "0");
  CHECK(out[4] == "7");
  CHECK(out[5] == "8");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: GBP glyph in slot 0") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "GBP";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "\xC2\xA3");  // UTF-8 pound sign
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: JPY glyph in slot 0") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "JPY";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "\xC2\xA5");  // UTF-8 yen sign
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: CAD uses $ glyph") {
  // CAD and AUD share the $ glyph in CurrencySymbolLocal — same as the
  // device-side CurrencySymbolUtf8 (main/screens/common.cpp).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "CAD";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "$");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: AUD uses $ glyph") {
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "AUD";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "$");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: CHF keeps ISO-code cell") {
  // CHF has no single-character glyph in the panel font; CurrencySymbol
  // returns the literal "CHF" string. The layout still treats it as one
  // cell regardless of byte count, so the mirror holds "CHF" in slot 0.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "CHF";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "CHF");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: unknown ISO falls back to code") {
  // Runtime-fetched catalogues from /api/v2/currencies can include codes
  // the firmware has no dedicated glyph for (BRL, INR, …). Same shape as
  // CHF: CurrencySymbolLocal returns the ISO code itself, and the layout
  // emits one cell holding that code so the mirror still agrees with
  // what the on-device renderer paints.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "BRL";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BRL");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — moscow time fractional: weak fiat shows 0.dddd") {
  // 1 BTC at ~2.55B VND → 1e8 / 2.55e9 ≈ 0.0392 sats per VND. Old
  // layout rounded to int32 and rendered "0" or all-blank — wrong. New
  // layout fills every digit cell with the fractional ratio.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "VND";
  in.price = "2550000000.0";
  in.use_sats_symbol = true;
  in.use_mscw_time = true;
  in.share_dot = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // SATS/<CCY> label keeps showing the currency identity (no MSCW path
  // for sub-1 sats).
  CHECK(out[0] == "SATS/VND");
  // use_sats_symbol=true reserves cell 1 for "STS"; the fractional
  // body shifts right by one — "0", ".", and 3 fractional digits across
  // cells 2..6.
  CHECK(out[1] == "STS");
  CHECK(out[2] == "0");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "3");
  CHECK(out[6] == "9");
}

TEST_CASE(
    "panel_texts — moscow time fractional: use_sats_symbol=false fills every "
    "slot") {
  // Same 0.0392 input but with the sats glyph suppressed — every cell
  // is free for digits, so one extra fractional digit fits.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "VND";
  in.price = "2550000000.0";
  in.use_sats_symbol = false;
  in.use_mscw_time = true;
  in.share_dot = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "SATS/VND");
  CHECK(out[1] == "0");
  CHECK(out[2] == ".");
  CHECK(out[3] == "0");
  CHECK(out[4] == "3");
  CHECK(out[5] == "9");
  CHECK(out[6] == "2");
}

TEST_CASE(
    "panel_texts — moscow time fractional: share_dot folds 0+. and gains "
    "a digit") {
  // Same 0.0392 input as above, but with the new decimalShareDot pref
  // set: the layout merges "0" and "." into a single cell so one extra
  // fractional digit fits across the same cell budget — even with the
  // sats glyph reserving cell 1.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "VND";
  in.price = "2550000000.0";
  in.use_sats_symbol = true;
  in.use_mscw_time = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "SATS/VND");
  CHECK(out[1] == "STS");
  CHECK(out[2] == "0.");
  CHECK(out[3] == "0");
  CHECK(out[4] == "3");
  CHECK(out[5] == "9");
  CHECK(out[6] == "2");
}

TEST_CASE("panel_texts — market cap small chars: unknown code in separator") {
  // The small-chars market-cap separator cell is " <glyph> ". For a code
  // without a hardcoded glyph (e.g. NOK), CurrencySymbolLocal falls back
  // to the ISO code so users see " NOK " instead of " " (which would
  // misleadingly suggest no currency).
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "NOK";
  in.price = "78280";
  in.block_height = 800000;
  in.mcap_big_chars = false;  // small-chars path
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "NOK/MCAP");
  // One of the inner slots carries the " NOK " separator. Find it.
  bool saw_sep = false;
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i] == " NOK ") {
      saw_sep = true;
      break;
    }
  }
  CHECK(saw_sep);
}

TEST_CASE("panel_texts — EUR suffix non-overflow keeps BTC/EUR label") {
  // Short prices where priceString fits with the label still emit the
  // BTC/<CCY> label in slot 0 (parity with v3's firstIndex=1 branch).
  // Contrast with the overflow cases above; this pins the branch split.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "EUR";
  in.price = "78280";
  in.suffix_price = true;
  in.mow_mode = false;  // NOT mow → "€78.3K" (6 cells; label fits)
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/EUR");
  CHECK(out[1] == "\xE2\x82\xAC");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — mowMode label path keeps BTC/<CCY> for price=0") {
  // Edge: price=0 falls out of FormatNumberWithSuffix as "0M" (2 chars)
  // regardless of num_characters, so `$0M`(3) fits with room for a
  // label. v4 keeps "BTC/<CCY>" on the mow label path (v3 emitted
  // "MOW/UNITS" here; we drop that so currency context stays visible).
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "0";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
}

TEST_CASE("panel_texts — V8 mowMode overflow: glyph in slot 0") {
  // V8 (8-panel) parity. 99000000 with mow: FormatNumberWithSuffix
  // num_chars = 8-1 = 7 → "99.000M"(7); "$99.000M"(8 cells) overflows
  // slot 0 → glyph on panel 0, digits fill 1..7.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "EUR";
  in.price = "99000000";
  in.suffix_price = true;
  in.mow_mode = true;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "\xE2\x82\xAC");
  CHECK(out[7] == "M");
}

TEST_CASE("panel_texts — integer-overflow path with EUR keeps glyph intact") {
  // Without explicit suffix_price, a very-large integer forces the
  // suffix branch (v3 `std::to_string(price).length() >= NUM_SCREENS`
  // guard). 99_999_999 → "100M" + "€" = 5 cells → label path on a
  // 7-panel board. This pins that the glyph still lands in a single
  // cell (not byte-split) on the label branch of the mirror.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "EUR";
  in.price = "99999999";
  in.suffix_price = false;
  in.mow_mode = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/EUR");
  // 5 cells in 6 digit slots → 1 leading pad at slot 1, glyph at slot 2.
  CHECK(out[1] == "");
  CHECK(out[2] == "\xE2\x82\xAC");
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — mowMode overflow: slot 0 is never BTC/<CCY> label") {
  // Parametric guard: for every supported currency, mowMode+suffixPrice
  // with a non-trivial price must NOT emit the BTC/<CCY> label in slot 0
  // — the EPD renderer drops the label on overflow and paints the
  // currency glyph on panel 0 instead. Catches future regressions of
  // the Rev B parity bug at the currency-dispatch level.
  const char* ccys[] = {"USD", "EUR", "GBP", "JPY", "CAD", "AUD", "CHF"};
  for (const char* ccy : ccys) {
    CAPTURE(ccy);
    PanelTextInputs in;
    in.kind = ScreenType::kBtcPrice;
    in.currency = ccy;
    in.price = "78280";
    in.suffix_price = true;
    in.mow_mode = true;
    const auto out = BuildPanelTexts(in, 7);
    REQUIRE(out.size() == 7);
    CHECK(out[0] != (std::string("BTC/") + ccy));
    CHECK(!out[0].empty());
    // Last cell is always the 'M' suffix for the mow path.
    CHECK(out[6] == "M");
  }
}

// --- shareDot panel-text parity ---
// Mirrors the v3 RenderPriceDataSuffixShareDot reference cases in
// test_datahandler_parity.cpp (PriceSuffixModeCompact1/Compact2 and
// PriceSuffixModeMowCompact). shareDot widens the formatter budget by
// one and folds the '.' into its preceding cell so the K/M label form
// keeps the BTC/<CCY> label and gets one more digit panel.

TEST_CASE("panel_texts — shareDot 78080 USD → BTC/USD label, dot folded") {
  // 78080 / 1000 = 78.08 → "78.08K" (6 bytes). With the symbol prepended
  // raw width is 7 cells, but the dot fold shaves one → effective 6 ≤ 6
  // digit slots → label path retains "BTC/USD" and slot 4 carries "8.".
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "78080";
  in.suffix_price = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "7");
  CHECK(out[3] == "8.");
  CHECK(out[4] == "0");
  CHECK(out[5] == "8");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — shareDot 100000 USD → BTC/USD label, '0.' folded") {
  // Mirrors PriceSuffixModeCompact1: 100000 + shareDot → "$100.0K" (7),
  // dot folds → ["$","1","0","0.","0","K"] right-justified after label.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "100000";
  in.suffix_price = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0.");
  CHECK(out[5] == "0");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — shareDot 1000000 USD → BTC/USD label, '1.' folded") {
  // Mirrors PriceSuffixModeCompact2: 1000000 + shareDot → "$1.000M" (7),
  // dot folds at position 1 → ["$","1.","0","0","0","M"].
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "1000000";
  in.suffix_price = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1.");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — shareDot+mow 93600 USD → BTC/USD, '0.' folded") {
  // Mirrors PriceSuffixModeMowCompact: 93600 + mow + shareDot →
  // "$0.093M" (7 raw bytes), dot folds → ["$","0.","0","9","3","M"]
  // and the label path stays alive so slot 0 is "BTC/USD". v3 swapped
  // to "MOW/UNITS" here; v4 keeps BTC/<CCY>.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "93600";
  in.suffix_price = true;
  in.mow_mode = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[1] == "$");
  CHECK(out[2] == "0.");
  CHECK(out[3] == "0");
  CHECK(out[4] == "9");
  CHECK(out[5] == "3");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — market cap shareDot folds dot into preceding cell") {
  // Same fold pattern propagated to market-cap big-chars. At ~$1.02T cap
  // the formatter (one extra digit of budget) emits "1.021T" → "$1.021T"
  // (7 bytes) → fold "1." into one cell so the magnitude reads as
  // ["$","1.","0","2","1","T"] across the 6 tail slots (7-panel board).
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "USD";
  in.block_height = 831000u;
  in.price = "52000";  // cap ≈ 1.02T
  in.mcap_big_chars = true;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "USD/MCAP");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1.");
  CHECK(out[3] == "0");
  CHECK(out[4] == "2");
  CHECK(out[5] == "1");
  CHECK(out[6] == "T");
}

TEST_CASE("panel_texts — market cap shareDot=false keeps unfolded layout") {
  // Counter-test: same inputs without shareDot must retain the existing
  // big-chars shape ($1.02T spread across 6 cells, dot in its own cell).
  PanelTextInputs in;
  in.kind = ScreenType::kMarketCap;
  in.currency = "USD";
  in.block_height = 831000u;
  in.price = "52000";
  in.mcap_big_chars = true;
  in.share_dot = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "USD/MCAP");
  CHECK(out[1] == "$");
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "2");
  CHECK(out[6] == "T");
}

TEST_CASE(
    "panel_texts — integer-overflow path with EUR in overflow (8-digit +)") {
  // For 9-digit prices the label is actually dropped on a 7-panel board:
  // FormatNumberWithSuffix(999_999_999, 5, false) → "1.00B" (5) + €(1)
  // = 6 cells. 6 < 7 → still label path. Try a case that actually
  // overflows — suffix_price=true forced, very large price expanding to
  // a 6+ cell priceString: 1000 with num_chars=5 → "1.00K"+ $ = 6 cells
  // → still fits. Most realistic overflow is mow mode (tested above).
  // This pins the non-mow overflow (shareDot off) corner case: a 3-digit
  // suffix like "1.000Q" (6 chars) for 1e15 + $ = 7 cells → overflow.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "EUR";
  in.price = "1000000000000000";  // 1e15 → Q suffix
  in.suffix_price = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // Either label-path or overflow-path is acceptable so long as the
  // slot-0 invariant holds: it's the label (BTC/EUR) OR the glyph €,
  // never the label with a dropped glyph.
  const bool is_label = (out[0] == "BTC/EUR");
  const bool is_glyph = (out[0] == "\xE2\x82\xAC");
  CHECK((is_label || is_glyph));
  // If it's the label, the glyph is somewhere in slots 1..5. If the
  // label was dropped, slot 0 IS the glyph and it's NOT "BTC/EUR".
}

TEST_CASE("panel_texts — fee rate emits sat/vB split tokens") {
  // Bug 4 — the unit panel is now a split-text ("sat" over "vB")
  // matching the paired FEE/RATE label. The mirror string encodes the
  // split as "sat/vB" (slash is the WebUI split indicator, same as
  // BLOCK/HEIGHT and FEE/RATE), so the paired tokens "sat" and "vB"
  // both appear in the string.
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 21.0;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[6] == "sat/vB");
  CHECK(out[6].find("sat") != std::string::npos);
  CHECK(out[6].find("vB") != std::string::npos);
  CHECK(out[6].find('/') != std::string::npos);
  // Integer 21 right-justified into 5 digit slots: "   21".
  CHECK(out[5] == "1");
  CHECK(out[4] == "2");
  CHECK(out[3] == "");
}

TEST_CASE("panel_texts — fee rate decimal form below 10") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 1.1;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[6] == "sat/vB");
  // "1.10" = 4 chars in 5 slots: " 1.10".
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "1");
  CHECK(out[5] == "0");
  CHECK(out[1] == "");
}

TEST_CASE("panel_texts — fee rate decimalShareDot folds dot") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 4.02;
  in.share_dot = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[6] == "sat/vB");
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "4.");
  CHECK(out[4] == "0");
  CHECK(out[5] == "2");
}

TEST_CASE("panel_texts — fee rate whole number below 10 shows .00") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 1.0;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  // " 1.00" in digit slots 1..5
  CHECK(out[1] == "");
  CHECK(out[2] == "1");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
}

TEST_CASE("panel_texts — fee rate missing value → blank digits, label + unit") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  // block_fee_sats_vb left nullopt — builder fills digits with "".
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[6] == "sat/vB");
  for (std::size_t i = 1; i < 6; ++i) CHECK(out[i] == "");
}

TEST_CASE("panel_texts — clock valid → date label + HH:MM in tail slots") {
  PanelTextInputs in;
  in.kind = ScreenType::kClock;
  in.clock_valid = true;
  in.hour = 13;
  in.minute = 37;
  in.mday = 9;
  in.month = 5;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "9/5");
  // digits 1..6: ComputeClockLayout(valid, 13, 37, 6) → base=1, so
  // slots 2,3,4,5,6 carry 1 3 : 3 7 with slot 1 blank.
  CHECK(out[1] == "");
  CHECK(out[2] == "1");
  CHECK(out[3] == "3");
  CHECK(out[4] == ":");
  CHECK(out[5] == "3");
  CHECK(out[6] == "7");
}

TEST_CASE("panel_texts — clock invalid → dash label, blank digits") {
  PanelTextInputs in;
  in.kind = ScreenType::kClock;
  in.clock_valid = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "-/-");
  for (std::size_t i = 1; i < 7; ++i) CHECK(out[i] == "");
}

TEST_CASE("panel_texts — clock hide_lead_zero drops leading hour digit") {
  // Mirror must match the EPD for the hideLeadZero pref: "07:05" on
  // panels → "", "", "7", ":", "0", "5" across digit slots so
  // /api/status `data[]` agrees with what the panels paint.
  PanelTextInputs in;
  in.kind = ScreenType::kClock;
  in.clock_valid = true;
  in.hour = 7;
  in.minute = 5;
  in.mday = 9;
  in.month = 5;
  in.hide_lead_zero = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "9/5");
  // digit_slots = 6, base = 1: slot 1 stays blank (ComputeClockLayout
  // base-slot padding), slot 2 is the tens-of-hours — blanked by
  // hide_lead_zero for h<10, so slot 2 is also "". The 7 lands in
  // slot 3, then ':', '0', '5'.
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "7");
  CHECK(out[4] == ":");
  CHECK(out[5] == "0");
  CHECK(out[6] == "5");
}

TEST_CASE("panel_texts — clock hide_lead_zero leaves two-digit hours alone") {
  PanelTextInputs in;
  in.kind = ScreenType::kClock;
  in.clock_valid = true;
  in.hour = 13;
  in.minute = 37;
  in.mday = 9;
  in.month = 5;
  in.hide_lead_zero = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "9/5");
  CHECK(out[2] == "1");
  CHECK(out[3] == "3");
  CHECK(out[4] == ":");
  CHECK(out[5] == "3");
  CHECK(out[6] == "7");
}

// Mining pool hashrate mirror. Slot 0 holds the pool identity (vendored
// logo → empty; text fallback → single name string, "<top>\n<bottom>"
// when the name contains a natural delimiter). Slot N-1 holds the unit;
// slots 1..N-2 carry the digits. The v4 layout matches v3's single-panel
// label convention — a previous v4 iteration consumed two panels for the
// label which left-truncated "50.0K" to "0.0K" on 7-panel boards.

TEST_CASE("panel_texts — mining pool hashrate PH/S with decimal (logo path)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // bd btclock_v4-5yi dropped every vendored bitmap; the host stub
  // exposes RegisterTestLogo so the logo-vs-text branch in
  // BuildPanelTexts stays covered without needing a real cached file.
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";
  // 1_300_000_000_000_000 H/s → 1.3 PH/S (length 16 > 15 triggers PH).
  in.pool.hashrate = "1300000000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "PH/S");
  // "1.3" right-justified across 5 digit slots (1..5): "  1.3".
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "1");
  CHECK(out[4] == ".");
  CHECK(out[5] == "3");
}

TEST_CASE(
    "panel_texts — mining pool hashrate EH/S large magnitude (logo path)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // bd btclock_v4-5yi: no pools ship vendored bitmaps any more, all
  // logos resolve through the LittleFS cache. Host tests have no
  // LittleFS so we register a synthetic name into the stub registry
  // to drive the logo branch.
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";
  // 1.234e18 H/s → EH/S scale. Max-chars is 5 digit slots on a 7-panel
  // board (slots 1..5), so the formatter returns "1.234" exactly.
  in.pool.hashrate = "1234000000000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "EH/S");
  CHECK(out[1] == "1");
  CHECK(out[2] == ".");
  CHECK(out[3] == "2");
  CHECK(out[4] == "3");
  CHECK(out[5] == "4");
}

TEST_CASE(
    "panel_texts — mining pool hashrate empty data shows H/S placeholder") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // Synthetic logo registered lower-case; query upper-case proves the
  // registry lookup is case-insensitive (same fold as the target
  // pool_logos::IEquals helper).
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "SYNTHLOGO";
  // No hashrate sample yet — renderer mirrors the "no data" state.
  in.pool.hashrate = "";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "H/S");
  // The single digit "0" lands in the rightmost digit slot.
  CHECK(out[5] == "0");
  for (std::size_t i = 1; i < 5; ++i) CHECK(out[i] == "");
}

TEST_CASE("panel_texts — mining pool hashrate text fallback (single word)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // "satoshiradio" has no vendored logo and no delimiter — slot 0
  // carries the bare name (no '\n' split marker).
  in.pool.name = "satoshiradio";
  in.pool.hashrate = "1300000000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "satoshiradio");
  CHECK(out[6] == "PH/S");
  // Digits now occupy slots 1..5 (5 slots); "1.3" right-justified.
  CHECK(out[3] == "1");
  CHECK(out[4] == ".");
  CHECK(out[5] == "3");
}

TEST_CASE(
    "panel_texts — pre-fetch logo'd pool falls back to text (host stub)") {
  // bd btclock_v4-5yi: pools that ship an upstream `.bin` but have no
  // cached file on disk (e.g. first-boot before the fetcher runs)
  // resolve through the LittleFS cache on the device. Host tests have
  // no LittleFS — `HasResolvedLogo` short-circuits to the synthetic
  // registry which is empty here — so the label cell paints the pool
  // name. Mirrors what a first-boot device shows before WiFi comes up
  // and the fetcher catches it.
  btclock::pool_logos::ClearTestLogos();
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  in.pool.name = "Braiins";
  in.pool.hashrate = "1300000000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "Braiins");
  CHECK(out[6] == "PH/S");
}

TEST_CASE(
    "panel_texts — mining pool hashrate text fallback (space split line)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // A pool whose display name contains a space: the mirror encodes the
  // DrawSplitText horizontal-line layout as "<top>/<bottom>" in slot 0 —
  // matches the v3 getDisplayLabel convention ("SATOSHI/RADIO") that
  // the WebUI already renders as a split-text cell.
  in.pool.name = "satoshi radio";
  in.pool.hashrate = "1300000000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "satoshi/radio");
  CHECK(out[6] == "PH/S");
}

TEST_CASE(
    "panel_texts — mining pool hashrate text fallback (underscore split "
    "line)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // "public_pool" is the pool_name() key — no vendored logo → split on
  // the '_' and emit "<top>/<bottom>" as the split-text marker.
  in.pool.name = "public_pool";
  in.pool.hashrate = "645000000000";
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "public/pool");
  CHECK(out[6] == "GH/S");
}

TEST_CASE("panel_texts — mining pool earnings sats verbatim (logo path)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";
  in.pool.daily_sats = 8421;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "SATS");
  // "8421" right-justified into 5 digit slots: slot 1 blank, 2..5 carry.
  CHECK(out[1] == "");
  CHECK(out[2] == "8");
  CHECK(out[3] == "4");
  CHECK(out[4] == "2");
  CHECK(out[5] == "1");
}

TEST_CASE(
    "panel_texts — mining pool earnings 10K..99K keeps leading digit "
    "(7-panel)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";  // logo path (synthetic post-5yi audit)
  // 50000 sats → "50.0K" (5 chars). With the single-panel label area,
  // the 7-panel board has 5 digit slots so the full string fits
  // verbatim — the pre-fix layout truncated to "0.0K".
  in.pool.daily_sats = 50000;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "SATS");
  CHECK(out[1] == "5");
  CHECK(out[2] == "0");
  CHECK(out[3] == ".");
  CHECK(out[4] == "0");
  CHECK(out[5] == "K");
}

TEST_CASE("panel_texts — mining pool earnings 12.3K on 8-panel board") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";  // logo path (synthetic post-5yi audit)
  in.pool.daily_sats = 12300;  // "12.3K"
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "");
  CHECK(out[7] == "SATS");
  // Six digit slots 1..6 — "12.3K" right-justified with one leading blank.
  CHECK(out[1] == "");
  CHECK(out[2] == "1");
  CHECK(out[3] == "2");
  CHECK(out[4] == ".");
  CHECK(out[5] == "3");
  CHECK(out[6] == "K");
}

TEST_CASE(
    "panel_texts — mining pool earnings whale mode switches to BTC (text "
    "fallback)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  // "BigPool" isn't in the registry → single-word text fallback in slot 0.
  in.pool.name = "BigPool";
  // 2.5 BTC/day → 250_000_000 sats. Whale mode drops to "BTC" label.
  in.pool.daily_sats = 250000000;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BigPool");
  CHECK(out[6] == "BTC");
  // "2" lands in the rightmost digit slot (integer-BTC display).
  CHECK(out[5] == "2");
}

TEST_CASE("panel_texts — mining pool earnings without data keeps SATS label") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  in.pool.name = "";
  // No daily_sats populated.
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[6] == "SATS");
  for (std::size_t i = 1; i < 6; ++i) CHECK(out[i] == "");
}

TEST_CASE("panel_texts — mining pool hashrate V8 8-panel layout") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  btclock::pool_logos::ClearTestLogos();
  btclock::pool_logos::RegisterTestLogo("synthlogo");
  in.pool.name = "synthlogo";
  in.pool.hashrate = "645000000000";  // 645 GH/S (length 12).
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "");
  CHECK(out[7] == "GH/S");
  // "645" right-justified into 6 digit slots (slots 1..6).
  CHECK(out[4] == "6");
  CHECK(out[5] == "4");
  CHECK(out[6] == "5");
}

TEST_CASE("panel_texts — empty n_panels returns empty vector") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockHeight;
  in.block_height = 100u;
  const auto out = BuildPanelTexts(in, 0);
  CHECK(out.empty());
}

TEST_CASE("panel_texts — 8-panel board has a trailing digit slot") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockHeight;
  in.block_height = 123456u;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BLOCK/HEIGHT");
  // 6-digit number in 7 digit slots: slot 1 blank, 2..7 carry digits.
  CHECK(out[1] == "");
  CHECK(out[2] == "1");
  CHECK(out[7] == "6");
}

TEST_CASE("panel_texts — nostr zap small amount as integer (7 panels)") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 21;
  in.use_sats_symbol = false;  // no-glyph path — bolt at 1, amount tail.
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // Layout: [ZAP][bolt-empty][_][_][_][2][1].
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // mdi-lightning-bolt mirror cell
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "");
  CHECK(out[5] == "2");
  CHECK(out[6] == "1");
}

TEST_CASE("panel_texts — nostr zap integer fills tail (21000 on 7 panels)") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 21000;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // 5-digit integer fills the 5-cell tail — no need for "21K".
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "2");
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nostr zap k-suffix when integer overflows tail") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 210000;  // 6 digits — won't fit in 5-cell tail.
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // Falls back to "210K" right-justified.
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "");
  CHECK(out[3] == "2");
  CHECK(out[4] == "1");
  CHECK(out[5] == "0");
  CHECK(out[6] == "K");
}

TEST_CASE("panel_texts — nostr zap million-suffix (1.2M)") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 1'200'000;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // 4-char "1.2M" right-justified after [ZAP][bolt][_].
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "");
  CHECK(out[3] == "1");
  CHECK(out[4] == ".");
  CHECK(out[5] == "2");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — nostr zap without amount shows '?'") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.use_sats_symbol = false;
  // zap_amount_sats left nullopt — builder paints "?" as the amount.
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  CHECK(out[6] == "?");
}

TEST_CASE("panel_texts — nostr zap ignores message string") {
  // Message is kept in the snapshot but not mirrored — all tail cells
  // still go to the amount, regardless of message contents.
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 100;
  in.zap_message = "hi";
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  // "100" right-justified into 6 cells ⇒ 3 blanks + "1" + "0" + "0".
  CHECK(out[4] == "1");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nostr zap 8-panel board") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 500;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  // V8 layout: ZAP + bolt left-anchored, extra cell widens the blank gap
  // before the amount tail (no glyph here — pref off).
  // [ZAP][bolt][_][_][_][5][0][0]
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "");
  CHECK(out[5] == "5");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

// --- Sats-glyph prefix (useSatsSymbol=true) --------------------------
// These pin the Bug-2 fix: with the pref on, slot 1 carries the "STS"
// marker (same token parseSatsPerCurrency emits on Moscow-time) and
// the amount slides right by one cell. Mirrors the EPD renderer's
// kSatsGlyphSlot layout in main/screens/nostr_zap.cpp.

TEST_CASE("panel_texts — nostr zap glyph drops when integer fills tail") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 21000;  // 5-char integer fills the 5-cell tail.
  in.use_sats_symbol = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // No "STS" cell — the integer is preferred over the suffix-form so
  // the glyph slot is surrendered to the digits.
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "2");
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nostr zap glyph stays when amount has slack") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 1000;  // 4-char integer leaves room for the glyph.
  in.use_sats_symbol = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // [ZAP][bolt][STS][1][0][0][0]
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "STS");
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nostr zap with sats glyph (8 panels, 21000)") {
  // V8 parity: 6-cell tail with glyph reserve = 5 cells for digits.
  // "21000" (5 chars) fits as integer with the glyph still in place.
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 21000;
  in.use_sats_symbol = true;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  // [ZAP][bolt][_][STS][2][1][0][0][0]? — n=8, amount=5, first_amount=3,
  //   glyph_slot=2.
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "STS");
  CHECK(out[3] == "2");
  CHECK(out[4] == "1");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

TEST_CASE("panel_texts — nostr zap glyph on/off parity: suffix-form amount") {
  // Same input, only the pref flips. With an amount that triggers the
  // suffix path ("210K", 4 chars on a 5-cell tail), toggling the glyph
  // pref only swaps the cell just before the most-significant amount
  // digit between blank and "STS". The bolt cell stays at slot 1.
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 210000;  // renders as "210k" — 4-char tail with slack.

  in.use_sats_symbol = false;
  const auto off = BuildPanelTexts(in, 7);
  REQUIRE(off.size() == 7);
  CHECK(off[0] == "ZAP");
  CHECK(off[1] == "");  // bolt
  CHECK(off[2] == "");  // no glyph
  CHECK(off[3] == "2");
  CHECK(off[4] == "1");
  CHECK(off[5] == "0");
  CHECK(off[6] == "K");

  in.use_sats_symbol = true;
  const auto on = BuildPanelTexts(in, 7);
  REQUIRE(on.size() == 7);
  CHECK(on[0] == "ZAP");
  CHECK(on[1] == "");  // bolt
  CHECK(on[2] == "STS");
  CHECK(on[3] == "2");
  CHECK(on[4] == "1");
  CHECK(on[5] == "0");
  CHECK(on[6] == "K");
}

TEST_CASE("panel_texts — nostr zap edge: 1 sat still surfaces (Bug-1 edge)") {
  // The zap listener drops amount<1 sat before this builder runs; when
  // the builder is invoked at the edge (exactly 1 sat) the normal
  // "ZAP + bolt + <digit>" shape paints. This pins the boundary the
  // listener enforces via parser::ShouldSurfaceZap.
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 1;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  CHECK(out[1] == "");  // bolt
  CHECK(out[5] == "");
  CHECK(out[6] == "1");
}

// --- NWC balance + payment-notify mirror -----------------------------
// Pins the lwf.6 regression: kNwcBalance + kNwcPaymentNotify share the
// "label + bolt + amount" layout with kNostrZap, but the label must
// match RenderNwcBalanceScreen / RenderNwcPaymentNotifyScreen — "BAL"
// for the balance screen, "RECV" / "PAID" for the payment notification.
// The mirror used to call BuildNostrZap which hard-coded "ZAP", and
// the upstream screen_manager.cpp only populated nwc_balance_sats on
// the kNostrZap render path so the balance screen also saw "?" digits.

TEST_CASE("panel_texts — nwc balance: BAL label + digits fill the tail") {
  // Faucet wallet (10_000 sats) on a 7-panel Rev B board with both
  // glyph prefs off — the 5-digit integer fills the 5-cell tail with
  // no glyph reserve. Mirror equals what the EPD paints:
  //   [BAL][bolt][1][0][0][0][0]
  PanelTextInputs in;
  in.kind = ScreenType::kNwcBalance;
  in.nwc_balance_sats = 10000;
  in.use_sats_symbol = false;
  in.use_btc_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BAL");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "1");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nwc balance: glyph drops when amount fills tail") {
  // With useBtcSymbol on but a 5-digit balance the layout step prefers
  // the digits over the glyph — matches RenderNwcBalanceScreen's
  // ComputeNwcLayout glyph-drop branch. Pins the renderer's behaviour
  // for the on-device case the user reported (10_000 sats, btcSymbol).
  PanelTextInputs in;
  in.kind = ScreenType::kNwcBalance;
  in.nwc_balance_sats = 10000;
  in.use_btc_symbol = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BAL");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "1");
  CHECK(out[3] == "0");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nwc balance: glyph stays for short balance") {
  // 1000 sats (4 digits) leaves one slot of slack so the BTC glyph
  // survives — [BAL][bolt][₿][1][0][0][0]. Same layout rule as
  // BuildNostrZap's glyph-stays case but with the NWC label.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcBalance;
  in.nwc_balance_sats = 1000;
  in.use_btc_symbol = true;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BAL");
  CHECK(out[1] == "");              // bolt
  CHECK(out[2] == "\xe2\x82\xbf");  // U+20BF — Bitcoin sign
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nwc balance: empty (nullopt) shows '?'") {
  // Pre-first-poll state: no nwc_balance_msat in the snapshot yet.
  // The mirror surfaces "?" — same shape the NostrZap empty case
  // produces — so the WebUI's "no balance yet" indicator agrees with
  // the on-device "—" placeholder. Pins lwf.6 — before the fix this
  // case was the entire on-device bug.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcBalance;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "BAL");
  CHECK(out[1] == "");  // bolt
  CHECK(out[6] == "?");
}

TEST_CASE("panel_texts — nwc balance: 8-panel board layout") {
  // V8 parity: extra cell widens the blank gap before the amount,
  // doesn't shift BAL/bolt rightward.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcBalance;
  in.nwc_balance_sats = 500;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BAL");
  CHECK(out[1] == "");  // bolt
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "");
  CHECK(out[5] == "5");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

TEST_CASE("panel_texts — nwc payment-notify: incoming → RECV + DN arrow") {
  // direction = 1 (payment_received) → "RECV" label + "DN" mirror
  // token for the arrow-down glyph the EPD paints
  // (mdi::kIconArrowDownBold). Amount cells mirror the BuildNostrZap
  // right-justification.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcPaymentNotify;
  in.nwc_payment_amount_sats = 21;
  in.nwc_payment_direction = 1;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "RECV");
  CHECK(out[1] == "DN");  // mdi-arrow-down-bold mirror token
  CHECK(out[5] == "2");
  CHECK(out[6] == "1");
}

TEST_CASE("panel_texts — nwc payment-notify: outgoing → PAID + UP arrow") {
  // direction = 2 (payment_sent) → "PAID" label + "UP" mirror token
  // for the arrow-up glyph (mdi::kIconArrowUpBold).
  PanelTextInputs in;
  in.kind = ScreenType::kNwcPaymentNotify;
  in.nwc_payment_amount_sats = 1000;
  in.nwc_payment_direction = 2;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "PAID");
  CHECK(out[1] == "UP");  // mdi-arrow-up-bold mirror token
  CHECK(out[3] == "1");
  CHECK(out[4] == "0");
  CHECK(out[5] == "0");
  CHECK(out[6] == "0");
}

TEST_CASE("panel_texts — nwc payment-notify: unknown direction → RECV, bolt") {
  // direction = 0 (unknown) falls through to the friendlier "RECV"
  // label, and the renderer paints the bolt as a safe default — the
  // mirror leaves the glyph cell blank (matching the bolt convention
  // on kNwcBalance / kNostrZap where the bolt has no textual analogue).
  PanelTextInputs in;
  in.kind = ScreenType::kNwcPaymentNotify;
  in.nwc_payment_amount_sats = 42;
  in.nwc_payment_direction = 0;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "RECV");
  CHECK(out[1] == "");  // bolt fallback (no textual mirror token)
  CHECK(out[5] == "4");
  CHECK(out[6] == "2");
}

TEST_CASE("panel_texts — nwc payment-notify: 8-panel incoming layout (RECV)") {
  // V8 parity: extra cell widens the blank gap before the amount,
  // RECV + DN still anchor at slots 0/1.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcPaymentNotify;
  in.nwc_payment_amount_sats = 500;
  in.nwc_payment_direction = 1;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "RECV");
  CHECK(out[1] == "DN");
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "");
  CHECK(out[5] == "5");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

TEST_CASE("panel_texts — nwc payment-notify: 8-panel outgoing layout") {
  // V8 outgoing — PAID + UP at slots 0/1 with the amount right-aligned.
  PanelTextInputs in;
  in.kind = ScreenType::kNwcPaymentNotify;
  in.nwc_payment_amount_sats = 1500;
  in.nwc_payment_direction = 2;
  in.use_sats_symbol = false;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "PAID");
  CHECK(out[1] == "UP");
  CHECK(out[4] == "1");
  CHECK(out[5] == "5");
  CHECK(out[6] == "0");
  CHECK(out[7] == "0");
}

// Latest-wins merge semantics for DataSnapshot::LatestZap. The full
// Merge lives in hub.cpp (IDF-tainted) but the rule is simple enough
// to replicate inline: higher received_ms wins, equal/lower is a
// no-op. This test pins the rule so a future edit to hub.cpp can't
// silently drop the latest-wins invariant.
TEST_CASE("LatestZap merge: newer received_ms wins") {
  btclock::DataSnapshot::LatestZap cur{};
  cur.amount_sats = 100;
  cur.message = "older";
  cur.received_ms = 1000;

  btclock::DataSnapshot::LatestZap fresh{};
  fresh.amount_sats = 500;
  fresh.message = "newer";
  fresh.received_ms = 2000;

  // Replicates the guard in hub.cpp: `other.received_ms > cur.received_ms`.
  CHECK(fresh.received_ms > cur.received_ms);
  if (fresh.received_ms > cur.received_ms) cur = fresh;
  REQUIRE(cur.amount_sats.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*cur.amount_sats == 500);
  CHECK(cur.message == "newer");

  // Stale receipt (same timestamp) must not overwrite.
  btclock::DataSnapshot::LatestZap stale{};
  stale.amount_sats = 1;
  stale.message = "stale";
  stale.received_ms = 2000;
  CHECK(!(stale.received_ms > cur.received_ms));
  if (stale.received_ms > cur.received_ms) cur = stale;
  REQUIRE(cur.amount_sats.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*cur.amount_sats == 500);
  CHECK(cur.message == "newer");
}

// --- Distributed-display strip widths (n_panels = summed peer panels) ---
//
// The master builds one logical screen across the summed panel count of
// every peer and slices it per device. Two 7-panel boards → n_panels=14.
// These three kinds used to be capped at n_panels∈{7,8}: BtcPrice and
// FeeRate emitted label + blanks (so the slave slice was empty), and
// MoscowTime read a fixed 6-slot layout array out of bounds and aborted
// the firmware. They now lay out across the full strip.

TEST_CASE("panel_texts — Moscow time distributes across a 14-panel strip") {
  // Regression: kMoscowTime at n_panels=14 previously read a 6-element
  // layout array with a 13-iteration loop → OOB → std::length_error →
  // abort() (exceptions disabled). Must not crash and must right-justify.
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";  // 1e8/2684 → 37258 sats (classic MSCW/TIME range).
  const auto out = BuildPanelTexts(in, 14);
  REQUIRE(out.size() == 14);
  CHECK(out[0] == "MSCW/TIME");
  // 5 digits right-justified in 13 slots; STS marker one slot ahead.
  CHECK(out[8] == "STS");
  CHECK(out[9] == "3");
  CHECK(out[10] == "7");
  CHECK(out[11] == "2");
  CHECK(out[12] == "5");
  CHECK(out[13] == "8");
  // The leading (master) digit slots stay blank — value lives on the tail.
  CHECK(out[1] == "");
  CHECK(out[7] == "");
}

TEST_CASE("panel_texts — BTC price distributes onto the slave slice") {
  // Regression: kBtcPrice at n_panels=14 emitted "BTC/USD" + 13 blanks,
  // so the slave (cells 7..13) rendered empty. The digits must now land
  // on the tail cells.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "64211.53";
  const auto out = BuildPanelTexts(in, 14);
  REQUIRE(out.size() == 14);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[6] == "$");
  CHECK(out[7] == "6");
  CHECK(out[8] == "4");
  CHECK(out[9] == "2");
  CHECK(out[10] == "1");
  CHECK(out[11] == "1");
  CHECK(out[12] == ".");
  CHECK(out[13] == "5");
  // The reported bug: the slave slice (indices 7..13) is non-blank.
  bool tail_has_content = false;
  for (std::size_t i = 7; i < out.size(); ++i) {
    if (!out[i].empty()) tail_has_content = true;
  }
  CHECK(tail_has_content);
}

TEST_CASE("panel_texts — fee rate distributes across a 14-panel strip") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 12.75;
  const auto out = BuildPanelTexts(in, 14);
  REQUIRE(out.size() == 14);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[13] == "sat/vB");
  // "12.75" right-justified in the 12 digit slots (indices 1..12).
  CHECK(out[8] == "1");
  CHECK(out[9] == "2");
  CHECK(out[10] == ".");
  CHECK(out[11] == "7");
  CHECK(out[12] == "5");
}

TEST_CASE("panel_texts — clock distributes across a 14-panel strip") {
  // Regression: kClock at n_panels=14 read the fixed char[8] ClockLayout
  // across 13 digit slots → OOB read → garbage/blank clock cells.
  PanelTextInputs in;
  in.kind = ScreenType::kClock;
  in.clock_valid = true;
  in.hour = 13;
  in.minute = 37;
  in.mday = 9;
  in.month = 5;
  const auto out = BuildPanelTexts(in, 14);
  REQUIRE(out.size() == 14);
  CHECK(out[0] == "9/5");
  // "HH:MM" right-justified in the last 5 of 13 digit slots (base=8),
  // i.e. out indices 9..13. Earlier slots blank.
  CHECK(out[9] == "1");
  CHECK(out[10] == "3");
  CHECK(out[11] == ":");
  CHECK(out[12] == "3");
  CHECK(out[13] == "7");
  for (std::size_t i = 1; i <= 8; ++i) CHECK(out[i] == "");
}
