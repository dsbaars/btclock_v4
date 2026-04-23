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
