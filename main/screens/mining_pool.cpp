#include "screens/screens.hpp"

#include <array>
#include <cstring>
#include <string>

#include "screens/assets/pool_logos.hpp"
#include "screens/common.hpp"
#include "screens/panel_texts.hpp"

namespace btclock {

namespace {

// Reference chars for the split-text pool label. Uppercase + digits
// covers every display name the pool plugins register today; a narrower
// set would drop a baseline on names like "Ocean" vs "OCEAN".
constexpr const char* kPoolLabelRef =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Case-insensitive equality for two pool names. The polling sources
// report names in various casings ("Ocean", "OCEAN", "ocean"); comparing
// insensitively keeps the label panel from repainting on a casing flip.
bool SamePoolName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    const unsigned char la =
        (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + 32) : ca;
    const unsigned char lb =
        (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + 32) : cb;
    if (la != lb) return false;
  }
  return true;
}

// Split a pool name into two halves for the split-text fallback: if the
// name contains a natural delimiter (whitespace, '_'), split on the first
// occurrence; otherwise leave the right half empty so the renderer
// centres the single word across the panel. Shared with panel_texts.cpp
// via the same helper in that file — they must agree because /api/status
// `data[]` mirrors the EPD cell-for-cell.
struct PoolLabelSplit {
  std::string top;
  std::string bottom;  // empty → single-line render
};

PoolLabelSplit SplitPoolName(const std::string& name) {
  PoolLabelSplit out;
  if (name.empty()) return out;
  const auto sep = name.find_first_of(" \t_");
  if (sep == std::string::npos) {
    out.top = name;
    return out;
  }
  out.top = name.substr(0, sep);
  // Skip the separator itself; the Antonio subset doesn't render it.
  std::size_t rhs = sep + 1;
  while (rhs < name.size() &&
         (name[rhs] == ' ' || name[rhs] == '\t' || name[rhs] == '_')) {
    ++rhs;
  }
  out.bottom = name.substr(rhs);
  return out;
}

// Build the panel-0 PaintSlot for a pool. Vendored logo → kIconBitmap;
// split-text name fallback → kLabelSplit; single-word name → kLabel.
PaintSlot BuildPoolLabelSlot(const std::string& name) {
  if (const pool_logos::PoolLogo* logo = pool_logos::Lookup(name)) {
    PaintSlot slot{};
    slot.kind = PaintSlot::kIconBitmap;
    slot.bitmap = logo->bitmap;
    slot.bmp_w = logo->width;
    slot.bmp_h = logo->height;
    slot.ref_override = kPoolLabelRef;
    return slot;
  }
  if (name.empty()) {
    return PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
  }
  const PoolLabelSplit split = SplitPoolName(name);
  if (split.bottom.empty()) {
    // Single-word pool name: paint as one centred string. kLabel
    // renders at the label role + 54 pt, matches the pre-refactor
    // kPoolNamePx/kPoolLabelRef branch.
    PaintSlot slot{};
    slot.kind = PaintSlot::kLabel;
    slot.text = split.top;
    slot.ref_override = kPoolLabelRef;
    return slot;
  }
  // Two halves with separator line — same visual shape as BLOCK/HEIGHT
  // and FEE/RATE. Feed a "TOP/BOTTOM" into kLabelSplit (which splits on
  // the first '/'); the split-text path draws the 6 px pill-ended line
  // between halves.
  PaintSlot slot{};
  slot.kind = PaintSlot::kLabelSplit;
  slot.text = split.top + "/" + split.bottom;
  slot.ref_override = kPoolLabelRef;
  return slot;
}

// Build the trailing PaintSlot for a unit string. "PH/S" → kUnitSplit
// (split-text at unit role); "SATS" / "BTC" → kLabel (single-line at
// label role). The unit role renders at 54 pt via kUnitPx, which
// matches the pre-refactor kUnitSplitPx; kLabel also renders at 54 pt
// so single-token units stay on the same baseline.
PaintSlot BuildUnitSlot(const std::string& unit) {
  if (unit.empty()) {
    return PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
  }
  if (unit.find('/') != std::string::npos) {
    PaintSlot slot{};
    slot.kind = PaintSlot::kUnitSplit;
    slot.text = unit;
    slot.ref_override = kPoolLabelRef;
    return slot;
  }
  // Pre-refactor rendered single-token units through the unit font at
  // 54 pt; kLabel uses the label font at 54 pt. The two roles default
  // to the same Antonio family so hashes stay identical; if a future
  // fontName swap binds unit separately, single-token "SATS"/"BTC"
  // units would want their own kind — filed as a follow-up in bd.
  PaintSlot slot{};
  slot.kind = PaintSlot::kLabel;
  slot.text = unit;
  slot.ref_override = kPoolLabelRef;
  return slot;
}

// Right-justify a formatted string into a char array of digit slots.
// Spaces pad the head; overflow truncates the leading chars. Matches
// the panel_texts mirror byte-for-byte so WebUI /api/status and the EPD
// show the same content.
std::string RightJustifyDigits(const std::string& value,
                               std::size_t digit_slots) {
  if (digit_slots == 0) return {};
  if (value.size() >= digit_slots) {
    return value.substr(value.size() - digit_slots);
  }
  return std::string(digit_slots - value.size(), ' ') + value;
}

// Ref chars shared by the digit panels. Dot-inclusive so "1.3" and "123"
// share a baseline — the hashrate value can include a decimal point
// depending on magnitude. The earnings screen uses a different set
// that also covers the K/M/B suffix letters (kEarnDigitRef below).
constexpr const char* kHashDigitRef = "0123456789.";
constexpr const char* kEarnDigitRef = "0123456789.KMB";

}  // namespace

template <size_t N>
void RenderMiningPoolHashrateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const DataSnapshot::PoolStats& pool,
    const DataSnapshot::PoolStats& prev_pool) {
  static_assert(N >= 7, "mining-pool hashrate layout needs at least 7 panels");
  // Panel 0 holds the pool logo (or split-text fallback), panel N-1 holds
  // the unit label, so the digit area is N-2 slots starting at panel 1.
  // v3-aligned single-panel label; previous two-panel layout caused
  // "50.0K" to left-truncate to "0.0K" on 7-panel boards.
  constexpr std::size_t kDigitPanels = N - 2;
  constexpr std::size_t kFirstDigitPanel = 1;

  // Full refresh when the previous pool snapshot was empty (first paint
  // after a slot change) or when the pool identity flipped under us
  // (settings change mid-session).
  const bool full_refresh =
      prev_pool.name.empty() ||
      (!pool.name.empty() && !SamePoolName(pool.name, prev_pool.name));

  const MiningPoolHashrateLayout now_layout = LayoutMiningPoolHashrate(
      pool.hashrate,
      static_cast<unsigned int>(kDigitPanels ? kDigitPanels : 1));
  const MiningPoolHashrateLayout prev_layout =
      full_refresh
          ? MiningPoolHashrateLayout{}
          : LayoutMiningPoolHashrate(
                prev_pool.hashrate,
                static_cast<unsigned int>(kDigitPanels ? kDigitPanels : 1));

  const std::string now_digits =
      RightJustifyDigits(now_layout.value, kDigitPanels);
  const std::string prev_digits =
      full_refresh ? std::string()
                   : RightJustifyDigits(prev_layout.value, kDigitPanels);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — logo or name-split label.
  slots[0] = BuildPoolLabelSlot(pool.name);
  update[0] = full_refresh;

  // Digit panels 1..N-2. ' ' → blank cell (kDigit short-circuits on ' ').
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    const std::size_t panel_idx = kFirstDigitPanel + i;
    const char c = now_digits[i];
    PaintSlot slot{};
    slot.kind = PaintSlot::kDigit;
    slot.text = std::string(1, c);
    slot.ref_override = kHashDigitRef;
    slots[panel_idx] = slot;
    update[panel_idx] =
        full_refresh || (i < prev_digits.size()
                             ? now_digits[i] != prev_digits[i]
                             : now_digits[i] != ' ');
  }

  // Panel N-1 — unit label.
  slots[N - 1] = BuildUnitSlot(now_layout.unit);
  update[N - 1] = full_refresh || now_layout.unit != prev_layout.unit;

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template <size_t N>
void RenderMiningPoolEarningsScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const DataSnapshot::PoolStats& pool,
    const DataSnapshot::PoolStats& prev_pool) {
  static_assert(N >= 7, "mining-pool earnings layout needs at least 7 panels");
  // Same (label, digits…, unit) shape as the hashrate screen: 1 label +
  // N-2 digits + 1 unit. Previous 2-label layout truncated the 5-char
  // "50.0K" to "0.0K" on a 4-slot digit area.
  constexpr std::size_t kDigitPanels = N - 2;
  constexpr std::size_t kFirstDigitPanel = 1;

  const bool full_refresh =
      prev_pool.name.empty() ||
      (!pool.name.empty() && !SamePoolName(pool.name, prev_pool.name));

  const MiningPoolEarningsLayout now_layout =
      LayoutMiningPoolEarnings(pool.daily_sats.value_or(-1));
  const MiningPoolEarningsLayout prev_layout =
      full_refresh
          ? MiningPoolEarningsLayout{}
          : LayoutMiningPoolEarnings(prev_pool.daily_sats.value_or(-1));

  const std::string now_value =
      now_layout.valid ? now_layout.value : std::string();
  const std::string prev_value =
      prev_layout.valid ? prev_layout.value : std::string();
  const std::string now_digits = RightJustifyDigits(now_value, kDigitPanels);
  const std::string prev_digits =
      full_refresh ? std::string()
                   : RightJustifyDigits(prev_value, kDigitPanels);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  slots[0] = BuildPoolLabelSlot(pool.name);
  update[0] = full_refresh;

  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    const std::size_t panel_idx = kFirstDigitPanel + i;
    const char c = now_digits[i];
    PaintSlot slot{};
    slot.kind = PaintSlot::kDigit;
    slot.text = std::string(1, c);
    slot.ref_override = kEarnDigitRef;
    slots[panel_idx] = slot;
    update[panel_idx] =
        full_refresh || (i < prev_digits.size()
                             ? now_digits[i] != prev_digits[i]
                             : now_digits[i] != ' ');
  }

  // Unit label: "SATS" by default; "BTC" when the whale-mode branch
  // fires in LayoutMiningPoolEarnings. Invalid data still paints "SATS"
  // so users see an expected unit on the trailing panel.
  const std::string unit =
      now_layout.valid ? now_layout.unit_label : std::string("SATS");
  const std::string prev_unit =
      prev_layout.valid ? prev_layout.unit_label : std::string("SATS");
  slots[N - 1] = BuildUnitSlot(unit);
  update[N - 1] = full_refresh || unit != prev_unit;

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderMiningPoolHashrateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::PoolStats&,
    const DataSnapshot::PoolStats&);
template void RenderMiningPoolHashrateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::PoolStats&,
    const DataSnapshot::PoolStats&);
template void RenderMiningPoolEarningsScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::PoolStats&,
    const DataSnapshot::PoolStats&);
template void RenderMiningPoolEarningsScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::PoolStats&,
    const DataSnapshot::PoolStats&);

}  // namespace btclock
