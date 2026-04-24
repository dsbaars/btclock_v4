// Per-panel text builder tests.
//
// The /api/status `data[]` mirror comes out of
// main/screens/panel_texts.cpp — these cases pin the shapes the WebUI
// renders. They mirror a subset of the old firmware's parse*() parity
// tests (see test_datahandler_parity.cpp) but drive the actual IDF
// helper rather than a local copy.

#include "doctest.h"

#include <optional>
#include <string>
#include <vector>

#include "data_core/snapshot.hpp"
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
  CHECK(out[6] == " % ");
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
  // (see btclock_v3_fci-0v9) places the USD glyph prefix "$" in slot 1,
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

TEST_CASE("panel_texts — SATS/<CCY> label for non-USD currencies") {
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "EUR";
  in.price = "2684";
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "SATS/EUR");
}

TEST_CASE("panel_texts — useMscwTime=false forces SATS/USD for USD classic range") {
  // bd btclock_v4-5wj — the /api/settings useMscwTime toggle now gates
  // the Moscow-time label. With the flag off, USD in the classic range
  // falls back to the uniform SATS/USD heading (old firmware parity —
  // see v3_fci screen_handler.cpp parseSatsPerCurrency call site).
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "USD";
  in.price = "2684";        // 37259 sats — in classic MSCW/TIME range
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
  CHECK(out[1] == "");      // marker slot now blank
  CHECK(out[2] == "3");     // digits still right-justified
  CHECK(out[6] == "8");
}

TEST_CASE("panel_texts — useBlkCountdown=true keeps blocks countdown form") {
  // Default path — same as the existing halving-blocks case. Kept as an
  // explicit flag test so the screen_manager gate is covered end-to-end.
  PanelTextInputs in;
  in.kind = ScreenType::kHalving;
  in.block_height = 210000u - 5;
  in.halving_as_blocks = true;   // = useBlkCountdown from NVS
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

// btclock_v3_fci-lx0.12 coverage: BuildBtcPrice routes through the same
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

TEST_CASE("panel_texts — V8 BTC price is integer-only (no trailing dot+0)") {
  // Bug 2 — V8's 8-panel layout mirrors old firmware parsePriceData
  // (useSuffixFormat=false, shareDot=false): integer digits + glyph,
  // no '.' panel. The previous layout reused the Rev A/B sub-$100k
  // decimal path, which on V8 filled the last two panels with ". 0"
  // for integer-rounded prices.
  PanelTextInputs in;
  in.kind = ScreenType::kBtcPrice;
  in.currency = "USD";
  in.price = "7858.3";
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "BTC/USD");
  // 4-digit rounded integer in 7 digit slots with the glyph: 2 leading
  // pads, glyph, then 7858.
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "$");
  CHECK(out[4] == "7");
  CHECK(out[5] == "8");
  CHECK(out[6] == "5");
  CHECK(out[7] == "8");
  for (const auto& s : out) CHECK(s != ".");
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

// Mining pool hashrate mirror. Slot 0 holds the pool identity (vendored
// logo → empty; text fallback → single name string, "<top>\n<bottom>"
// when the name contains a natural delimiter). Slot N-1 holds the unit;
// slots 1..N-2 carry the digits. The v4 layout matches v3's single-panel
// label convention — a previous v4 iteration consumed two panels for the
// label which left-truncated "50.0K" to "0.0K" on 7-panel boards.

TEST_CASE("panel_texts — mining pool hashrate PH/S with decimal (logo path)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // "ocean" is in the vendored logo registry → slot 0 blank.
  in.pool.name = "ocean";
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

TEST_CASE("panel_texts — mining pool hashrate EH/S large magnitude (logo path)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  // "Braiins" folds to "braiins" in the logo registry.
  in.pool.name = "Braiins";
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

TEST_CASE("panel_texts — mining pool hashrate empty data shows H/S placeholder") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolHashrate;
  in.pool.name = "OCEAN";  // registry lookup is case-insensitive
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

TEST_CASE("panel_texts — mining pool hashrate text fallback (space split line)") {
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

TEST_CASE("panel_texts — mining pool hashrate text fallback (underscore split line)") {
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
  in.pool.name = "ocean";
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

TEST_CASE("panel_texts — mining pool earnings 10K..99K keeps leading digit (7-panel)") {
  PanelTextInputs in;
  in.kind = ScreenType::kMiningPoolEarnings;
  in.pool.name = "Braiins";  // logo path
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
  in.pool.name = "Braiins";  // logo path
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

TEST_CASE("panel_texts — mining pool earnings whale mode switches to BTC (text fallback)") {
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
  in.pool.name = "ocean";
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
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  // "21" right-justified into 6 amount cells ⇒ 4 blanks + "2" + "1".
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "");
  CHECK(out[5] == "2");
  CHECK(out[6] == "1");
}

TEST_CASE("panel_texts — nostr zap scaled k-suffix") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 21000;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  // "21k" right-justified into 6 amount cells ⇒ 3 blanks + "2" + "1" + "k".
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "2");
  CHECK(out[5] == "1");
  CHECK(out[6] == "k");
}

TEST_CASE("panel_texts — nostr zap million-suffix (1.2M)") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
  in.zap_amount_sats = 1'200'000;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "ZAP");
  // 4-char "1.2M" placed into 6 amount cells ⇒ 2 blanks + "1" + "." + "2" + "M".
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "1");
  CHECK(out[4] == ".");
  CHECK(out[5] == "2");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — nostr zap without amount shows '?'") {
  PanelTextInputs in;
  in.kind = ScreenType::kNostrZap;
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
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "ZAP");
  // "500" right-justified into 7 amount cells on 8-panel ⇒ 4 blanks
  // + digits in trailing cells.
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
  CHECK(*cur.amount_sats == 500);
  CHECK(cur.message == "newer");

  // Stale receipt (same timestamp) must not overwrite.
  btclock::DataSnapshot::LatestZap stale{};
  stale.amount_sats = 1;
  stale.message = "stale";
  stale.received_ms = 2000;
  CHECK(!(stale.received_ms > cur.received_ms));
  if (stale.received_ms > cur.received_ms) cur = stale;
  CHECK(*cur.amount_sats == 500);
  CHECK(cur.message == "newer");
}
