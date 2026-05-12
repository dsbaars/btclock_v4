// Pure-logic builder for the auto-rotate traversal sequence.
//
// Two inputs drive the rotation walk:
//   - `screenOrder` NVS CSV (user-editable, from the WebUI's drag-reorder).
//   - `screen<id>Visible` NVS booleans (per-screen toggle).
// Plus the active currency list (already filtered to `actCurrencies`).
//
// Output: a vector of ScreenManager slot indices — one entry per auto-
// rotate step — in traversal order. Per-currency kinds expand inline to
// one entry per active currency, in `currencies` order. Disabled screens
// (not in enabled predicate) are dropped. The fee-rate slot sits at the
// end exactly when the user's screenOrder puts api_id 6 at that position;
// if absent, it doesn't appear in rotation (user disabled it).
//
// Fallback: when `screenOrder` is empty (fresh device), the builder emits
// every slot 0..SlotCount-1 in index order — matches the pre-screenOrder
// rotation and keeps upgrades from an older firmware seamless.
//
// Kept in a header so both the ScreenManager (device) and host tests
// (no ESP-IDF) can link the same implementation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/screen_slot_map.hpp"

namespace btclock {
namespace rotation_plan {

// Predicate signature: `IsEnabled(api_id) -> bool`. Host tests install a
// lambda over a std::map; the device wires it to a closure over NVS.
using EnabledFn = std::function<bool(int api_id)>;

// Returns true when the kind has one slot per currency (not agnostic).
inline bool IsPerCurrencyKind(int api_id) {
  switch (api_id) {
    case slot_map::kApiIdMoscowTime:
    case slot_map::kApiIdBtcPrice:
    case slot_map::kApiIdMarketCap:
      return true;
    default:
      return false;
  }
}

// Base (currency-agnostic) slot for a kind. Returns -1 for per-currency
// kinds (caller must expand) or unknown ids.
inline int AgnosticSlotForApiId(int api_id, std::size_t currency_count) {
  switch (api_id) {
    case slot_map::kApiIdBlockHeight:
      return 0;
    case slot_map::kApiIdClock:
      return 1;
    case slot_map::kApiIdHalving:
      return 2;
    case slot_map::kApiIdBitcoinSupply:
      return 3;
    case slot_map::kApiIdMiningPoolHashrate:
      return 4;
    case slot_map::kApiIdMiningPoolEarnings:
      return 5;
    case slot_map::kApiIdBitaxeHashrate:
      return 6;
    case slot_map::kApiIdBitaxeBestDiff:
      return 7;
    case slot_map::kApiIdNwcBalance:
      return 8;
    case slot_map::kApiIdBlockFeeRate:
      return currency_count == 0
                 ? -1
                 : static_cast<int>(slot_map::SlotCount(currency_count) - 1);
    default:
      return -1;
  }
}

// Expand a single api_id into 0..N slot entries (N > 1 only for per-
// currency kinds) and append them to `out`. Unknown ids are dropped
// silently — matches the old-firmware "unknown key in screenOrder" path
// which ignored rather than bailed.
inline void ExpandApiIdInto(int api_id, std::size_t currency_count,
                            std::vector<std::size_t>& out) {
  if (IsPerCurrencyKind(api_id)) {
    // Per-currency stride: slot = kAgnosticSlots + 3*k + offset
    std::size_t offset = 0;
    switch (api_id) {
      case slot_map::kApiIdMoscowTime:
        offset = 0;
        break;
      case slot_map::kApiIdBtcPrice:
        offset = 1;
        break;
      case slot_map::kApiIdMarketCap:
        offset = 2;
        break;
    }
    for (std::size_t k = 0; k < currency_count; ++k) {
      out.push_back(slot_map::kAgnosticSlots + slot_map::kPerCurrencySlots * k +
                    offset);
    }
    return;
  }
  const int slot = AgnosticSlotForApiId(api_id, currency_count);
  if (slot < 0) return;
  out.push_back(static_cast<std::size_t>(slot));
}

// Parse the screenOrder CSV into api_ids. Whitespace / empty items are
// skipped. Invalid tokens are dropped silently.
//
// std::stoi would throw on a malformed token, but ESP-IDF builds with
// -fno-exceptions (see component.mk cxxflags) — so the parse is a hand-
// written digit walker instead. Matches the old-firmware resilience to
// a stray CSV entry: unknown / non-numeric tokens are skipped, not
// fatal.
inline std::vector<int> ParseScreenOrderCsv(const std::string& csv) {
  std::vector<int> out;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    // Trim leading whitespace (defensive — the WebUI emits dense CSV,
    // but a user editing the NVS blob by hand could leave spaces).
    std::size_t start = 0;
    while (start < item.size() && (item[start] == ' ' || item[start] == '\t')) {
      ++start;
    }
    if (start == item.size()) continue;
    bool negative = false;
    if (item[start] == '-') {
      negative = true;
      ++start;
    } else if (item[start] == '+') {
      ++start;
    }
    int value = 0;
    bool any_digit = false;
    bool bad_char = false;
    for (std::size_t i = start; i < item.size(); ++i) {
      const char c = item[i];
      if (c == ' ' || c == '\t') break;  // trailing ws ends the number
      if (c < '0' || c > '9') {
        bad_char = true;
        break;
      }
      value = value * 10 + (c - '0');
      any_digit = true;
    }
    if (!any_digit || bad_char) continue;
    out.push_back(negative ? -value : value);
  }
  return out;
}

// Build the traversal sequence.
//
// When `screen_order_csv` is empty, fall back to emitting every slot
// 0..SlotCount-1 in index order (the pre-screenOrder default). This is
// the backwards-compat path — a device that was factory-reset or was
// never PATCHed via the WebUI lands here. The `is_enabled` predicate
// still applies: a slot whose owning api_id is gated off (parent feature
// flag, per-screen `screen<id>Visible`, etc.) is dropped from the cold-
// boot sequence too — otherwise a fresh device with `miningPoolStats=
// false` would auto-rotate onto the dormant mining-pool slots.
//
// When `screen_order_csv` is present, walk the user's order, dropping
// disabled entries and expanding per-currency kinds inline. Entries whose
// api_id isn't recognised are skipped. If the resulting sequence would
// be empty (everything disabled), fall back to slot 0 (BlockHeight) as a
// single-step rotation so the device still has something to display — a
// truly empty rotation would wedge the ScreenManager.
inline std::vector<std::size_t> BuildRotationSequence(
    const std::string& screen_order_csv, const EnabledFn& is_enabled,
    std::size_t currency_count) {
  std::vector<std::size_t> out;
  if (screen_order_csv.empty()) {
    const std::size_t total = slot_map::SlotCount(currency_count);
    for (std::size_t i = 0; i < total; ++i) {
      if (is_enabled) {
        const int api_id = slot_map::ApiIdForSlot(i, currency_count);
        if (api_id < 0 || !is_enabled(api_id)) continue;
      }
      out.push_back(i);
    }
    if (out.empty()) out.push_back(0);
    return out;
  }
  const std::vector<int> api_ids = ParseScreenOrderCsv(screen_order_csv);
  for (int api_id : api_ids) {
    if (is_enabled && !is_enabled(api_id)) continue;
    ExpandApiIdInto(api_id, currency_count, out);
  }
  if (out.empty()) out.push_back(0);
  return out;
}

// Sentinel for "no persisted lastSlot" — written by ResumeSlot callers
// when NVS returns the GetU32 default for a missing key. UINT32_MAX
// keeps the helper free of NVS / ESP-IDF dependencies so host tests
// can exercise the same code path the firmware boot runs.
inline constexpr std::uint32_t kNoSavedSlot = UINT32_MAX;

// Decide which slot the device should resume on after boot.
//
// Inputs mirror what `init_screen_manager` already has on hand after
// it builds the rotation sequence:
//   - `saved_slot`: persisted lastSlot from the runtime-state NVS
//     namespace, or `kNoSavedSlot` when absent (fresh device / first
//     boot after factory reset).
//   - `slot_count`: ScreenManager::slot_count() under the active
//     currency list — bounds the saved slot.
//   - `sequence`: the freshly-built rotation_sequence the device just
//     installed via `SetRotationSequence`.
//
// Returns the slot to call SetSlot(...) with, or std::nullopt to leave
// the constructor default (slot 0). Two-tier policy:
//   1. Restore `saved_slot` iff it's within bounds AND either matches
//      the trailing fee-rate slot (slot_count - 1, never appears in
//      `sequence` because the rotation expansion lists it explicitly)
//      or is present in `sequence`.
//   2. Otherwise land on `sequence[0]` so the user's first-in-order
//      screen paints on the very first frame instead of always
//      booting on block-height (slot 0) and auto-rotating off it
//      30 s later.
//   3. With an empty sequence, return std::nullopt — the constructor
//      default is the only sane landing spot.
inline std::optional<std::size_t> ResumeSlot(
    std::uint32_t saved_slot, std::size_t slot_count,
    const std::vector<std::size_t>& sequence) {
  auto in_sequence = [&sequence](std::size_t s) -> bool {
    for (std::size_t v : sequence)
      if (v == s) return true;
    return false;
  };
  const bool fee_slot = (slot_count > 0) && (saved_slot == slot_count - 1);
  if (saved_slot != kNoSavedSlot && saved_slot < slot_count &&
      (fee_slot || in_sequence(saved_slot))) {
    return static_cast<std::size_t>(saved_slot);
  }
  if (!sequence.empty()) return sequence.front();
  return std::nullopt;
}

// Minimal rotation-step simulator. ScreenManager's Next/Prev/MaybeAutoRotate
// bodies are essentially: `rotation_idx_ = (rotation_idx_ ± 1) % n` then
// "advance past any skip-predicate rejects". Exposing the inner loop as a
// pure-logic helper lets host tests pin the composed actCurrencies +
// screenOrder behaviour without linking the EPD / FreeRTOS body of
// ScreenManager. `skip(slot)` returns true to skip the candidate.
//
// Returns the new (slot, index) pair. `direction` is +1 or -1.
inline std::pair<std::size_t, std::size_t> StepSequence(
    const std::vector<std::size_t>& sequence, std::size_t current_idx,
    int direction, const std::function<bool(std::size_t)>& skip = nullptr) {
  const std::size_t n = sequence.size();
  if (n == 0) return {0, 0};
  std::size_t idx =
      (direction >= 0) ? (current_idx + 1) % n : (current_idx + n - 1) % n;
  for (std::size_t guard = 0; guard < n; ++guard) {
    if (!skip || !skip(sequence[idx])) return {sequence[idx], idx};
    idx = (direction >= 0) ? (idx + 1) % n : (idx + n - 1) % n;
  }
  return {sequence[idx], idx};
}

}  // namespace rotation_plan
}  // namespace btclock
