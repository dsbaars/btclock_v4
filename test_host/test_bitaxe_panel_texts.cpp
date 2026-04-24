// Panel-text mirror tests for the two Bitaxe screens.

#include <optional>
#include <string>
#include <vector>

#include "doctest.h"
#include "screens/panel_texts.hpp"

namespace {
using btclock::BuildPanelTexts;
using btclock::FormatBitaxeHashrate;
using btclock::PanelTextInputs;
using btclock::ScreenType;
using btclock::SplitBitaxeHashrate;
}  // namespace

TEST_CASE("FormatBitaxeHashrate — GH threshold") {
  CHECK(FormatBitaxeHashrate(0.0) == "0GH");
  CHECK(FormatBitaxeHashrate(450.0) == "450GH");
  CHECK(FormatBitaxeHashrate(1200.0) == "1.2TH");
  CHECK(FormatBitaxeHashrate(12'000.0) == "12TH");
  CHECK(FormatBitaxeHashrate(2'500'000.0) == "2.5PH");
}

TEST_CASE("SplitBitaxeHashrate — peels off magnitude suffix") {
  // Suffix comes off for the renderer to paint as "<suffix>/S"
  // split-text; value keeps its dot for sub-10 TH/PH decimals.
  const auto g = SplitBitaxeHashrate(527.0);
  CHECK(g.value == "527");
  CHECK(g.suffix == "GH");
  const auto t = SplitBitaxeHashrate(1200.0);
  CHECK(t.value == "1.2");
  CHECK(t.suffix == "TH");
  const auto t10 = SplitBitaxeHashrate(12'000.0);
  CHECK(t10.value == "12");
  CHECK(t10.suffix == "TH");
  const auto p = SplitBitaxeHashrate(1'500'000.0);
  CHECK(p.value == "1.5");
  CHECK(p.suffix == "PH");
  const auto zero = SplitBitaxeHashrate(0.0);
  CHECK(zero.value == "0");
  CHECK(zero.suffix == "GH");
}

TEST_CASE("panel_texts — bitaxe hashrate TH case, 7 panels") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeHashrate;
  in.bitaxe_hostname = "bitaxe.local";
  in.bitaxe_hashrate_ghs = 1200.0;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // Slot 0 blank (logo). Slots 1..N-2 carry the value "1.2" right-
  // justified across 5 digit slots (2 leading blanks). Slot N-1 is the
  // "TH/S" split-text unit cell — frees a digit slot vs the old
  // two-panel "T" "H" layout.
  CHECK(out[0] == "");
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "1");
  CHECK(out[4] == ".");
  CHECK(out[5] == "2");
  CHECK(out[6] == "TH/S");
}

TEST_CASE("panel_texts — bitaxe hashrate GH case, 7 panels") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeHashrate;
  in.bitaxe_hostname = "bitaxe.local";
  in.bitaxe_hashrate_ghs = 527.0;
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  // 527 GH — three digits right-justified into the 5-slot digit area.
  CHECK(out[0] == "");
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "5");
  CHECK(out[4] == "2");
  CHECK(out[5] == "7");
  CHECK(out[6] == "GH/S");
}

TEST_CASE("panel_texts — bitaxe hashrate PH case, 8 panels") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeHashrate;
  in.bitaxe_hostname = "bitaxe.local";
  in.bitaxe_hashrate_ghs = 1'500'000.0;
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  // 1.5 PH on V8 — "1.5" (3 cells) right-justified into the 6-slot
  // digit area; unit cell is "PH/S" in slot 7.
  CHECK(out[0] == "");
  CHECK(out[1] == "");
  CHECK(out[2] == "");
  CHECK(out[3] == "");
  CHECK(out[4] == "1");
  CHECK(out[5] == ".");
  CHECK(out[6] == "5");
  CHECK(out[7] == "PH/S");
}

TEST_CASE("panel_texts — bitaxe hashrate OFFLINE fallback, 8 panels") {
  // Empty hostname means "source disabled or not yet sampled" — both
  // renderer and mirror render the OFFLINE placeholder so the WebUI
  // and EPD agree on the user-visible state. OFFLINE spans the full
  // tail (including the would-be unit slot) — no "/S" suffix applies
  // when the device isn't reporting.
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeHashrate;
  // hostname left empty
  in.bitaxe_hashrate_ghs = 500.0;  // ignored — hostname dominates
  const auto out = BuildPanelTexts(in, 8);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == "");
  // "OFFLINE" is 7 chars, exactly filling the 7 tail slots 1..7.
  CHECK(out[1] == "O");
  CHECK(out[2] == "F");
  CHECK(out[3] == "F");
  CHECK(out[4] == "L");
  CHECK(out[5] == "I");
  CHECK(out[6] == "N");
  CHECK(out[7] == "E");
}

TEST_CASE("panel_texts — bitaxe best diff passes string through") {
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeBestDiff;
  in.bitaxe_hostname = "bitaxe.local";
  in.bitaxe_best_diff = std::string("15.6M");
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  // "15.6M" is 5 chars into 6 tail slots — one leading blank.
  CHECK(out[1] == "");
  CHECK(out[2] == "1");
  CHECK(out[3] == "5");
  CHECK(out[4] == ".");
  CHECK(out[5] == "6");
  CHECK(out[6] == "M");
}

TEST_CASE("panel_texts — bitaxe best diff OFFLINE when empty string") {
  // Edge case: hostname set but best_diff is empty (e.g. the pool
  // hasn't accepted a share yet). Renderer prefers OFFLINE over a
  // blank panel so the user doesn't see a half-painted screen.
  // "OFFLINE" is 7 codepoints; on 7 panels the tail is 6 slots so the
  // leading "O" is trimmed and tail reads F F L I N E across slots 1..6.
  PanelTextInputs in;
  in.kind = ScreenType::kBitaxeBestDiff;
  in.bitaxe_hostname = "bitaxe.local";
  in.bitaxe_best_diff = std::string();
  const auto out = BuildPanelTexts(in, 7);
  REQUIRE(out.size() == 7);
  CHECK(out[0] == "");
  CHECK(out[1] == "F");
  CHECK(out[2] == "F");
  CHECK(out[3] == "L");
  CHECK(out[4] == "I");
  CHECK(out[5] == "N");
  CHECK(out[6] == "E");
}
