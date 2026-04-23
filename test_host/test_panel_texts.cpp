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

TEST_CASE("panel_texts — SATS/<CCY> label for non-USD currencies") {
  PanelTextInputs in;
  in.kind = ScreenType::kMoscowTime;
  in.currency = "EUR";
  in.price = "2684";
  const auto out = BuildPanelTexts(in, 7);
  CHECK(out[0] == "SATS/EUR");
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

TEST_CASE("panel_texts — fee rate label + unit text, integer fee") {
  PanelTextInputs in;
  in.kind = ScreenType::kBlockFeeRate;
  in.block_fee_sats_vb = 21.0;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[6] == "sat/vB");
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
