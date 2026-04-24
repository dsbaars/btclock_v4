// Per-panel text mirror for `/api/status` `data[]`.
//
// The WebUI status panel echoes back what each EPD is currently showing
// as an array of strings. The old firmware built this out of
// `EPDManager::getCurrentContent()` — a buffer written by the `parse*()`
// helpers in lib/btclock/data_handler.cpp (label in slot 0, digit chars
// in the tail slots, unit text in the last slot for fee-rate).
//
// In the IDF port the renderers paint fonts directly; there is no string
// buffer they write to. Instead we recompute the same shape synthetically
// here from the snapshot data the renderer just consumed. The result
// matches the old firmware's slot layout closely enough that the WebUI
// renders it unchanged — see `idf_cpp_proto/test_host/test_panel_texts`
// for the parity cases that pin this.
//
// Pure-logic helper — no EPD, font, or FreeRTOS deps — so host tests can
// exercise it directly and `screen_manager.cpp` can call it on render.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "screens/screen_kind.hpp"

namespace btclock {

struct MiningPoolMirror {
  std::string name;        // Pool display label for slot 0 when no logo.
  std::string hashrate;    // Raw integer H/s string (empty = no sample).
  std::optional<int64_t> daily_sats;
};

// Bitaxe hashrate formatter. Given GH/s, emit a compact "<val><unit>"
// string with the smallest-fitting suffix (GH/TH/PH). Used by both the
// panel-text mirror and the EPD renderer so the WebUI and device agree
// to the last digit. Pure-logic so host tests can pin it.
std::string FormatBitaxeHashrate(double ghs);

// Split the FormatBitaxeHashrate output into its numeric value (e.g.
// "1.2", "527") and its two-char magnitude suffix ("GH" / "TH" / "PH").
// The renderer pairs the suffix with "/S" to paint a split-text unit
// panel; panel_texts emits the same "<suffix>/S" cell so /api/status
// mirrors what the EPD shows.
struct BitaxeHashrateParts {
  std::string value;   // digits + optional '.'
  std::string suffix;  // "GH" / "TH" / "PH"
};
BitaxeHashrateParts SplitBitaxeHashrate(double ghs);

// Minimum context a panel-text build needs. A trimmed view of
// `DataSnapshot` so host tests can construct inputs without including
// the full snapshot header (which pulls std::map<string,string> but no
// device deps — still, this keeps the helper's contract narrow).
struct PanelTextInputs {
  ScreenType kind = ScreenType::kBlockHeight;
  std::string currency;  // "" for currency-agnostic slots
  std::optional<uint32_t> block_height;
  std::optional<double>   block_fee_sats_vb;  // -1 / nullopt → "not yet"
  // Price is the raw string the data hub stores (e.g. "64211.53").
  // Empty = no price yet.
  std::string price;
  // Wall-clock pieces — validity flag gates everything. Only the time
  // screen reads these.
  bool clock_valid = false;
  int hour = 0;
  int minute = 0;
  int mday = 0;
  int month = 0;
  // Decoration flags. Mirror the on-device renderer's mode selection so
  // /api/status data[] matches what the EPDs actually paint — see
  // settings/pref_keys.hpp. Only the bits relevant to a given `kind`
  // are consulted by BuildPanelTexts; the rest are ignored.
  bool halving_as_blocks  = true;   // useBlkCountdown; false → "N/YRS N/DAYS..."
  bool supply_big_chars   = true;   // false → 3-digit-group small chars
  bool supply_percent     = false;  // overrides supply_big_chars when set
  bool mcap_big_chars     = true;   // false → 3-digit-group small chars
  // Moscow-time decoration. `use_sats_symbol=false` replaces the "STS"
  // marker cell with a blank, matching the old firmware's
  // parseSatsPerCurrency(..., useSatsSymbol=false) branch.
  bool use_sats_symbol    = true;
  // `use_mscw_time=false` forces the label to SATS/<CCY> even for the
  // classic-range USD case. Default true preserves the legacy MSCW/TIME
  // label on USD in the classic range.
  bool use_mscw_time      = true;
  // Mining-pool stats. Only consulted by the MiningPool* screen kinds.
  MiningPoolMirror pool{};
  // Bitaxe screens. Empty hostname means "no sample yet" — mirror
  // renders the OFFLINE fallback so the WebUI echoes what the panel
  // paints. Values are the raw ones the data source wrote; the
  // builder picks the hashrate suffix via FormatBitaxeHashrate.
  std::string bitaxe_hostname;
  std::optional<double> bitaxe_hashrate_ghs;
  std::optional<std::string> bitaxe_best_diff;
  // Zap notification inputs. Populated only when kind==kNostrZap;
  // BuildNostrZap emits "ZAP" in slot 0 and the scaled amount
  // right-justified across the remaining slots. `zap_message` is kept
  // for future use (a message-capable screen can consume it without
  // re-plumbing the relay listener) but is not currently rendered.
  std::optional<int64_t> zap_amount_sats;
  std::string zap_message;
};

// Build the `data[]` array, one string per panel, for `n_panels` panels.
// Slot 0 is the label (e.g. "BLOCK/HEIGHT"); subsequent slots carry the
// digit / separator characters; the last slot carries the unit text when
// the screen has one (fee-rate → "sat/vB"). Matches the old firmware's
// parse*()-based layout closely; renderer differences are noted in
// panel_texts.cpp next to each case.
std::vector<std::string> BuildPanelTexts(const PanelTextInputs& in,
                                         std::size_t n_panels);

}  // namespace btclock
