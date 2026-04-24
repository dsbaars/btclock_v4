#include "screens/screens.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "screens/assets/bitaxe_logo.hpp"
#include "screens/common.hpp"
#include "screens/panel_texts.hpp"

namespace btclock {
namespace {

// Ref chars for the unit split-text. Uppercase + '/' + 'S' covers
// "GH/S", "TH/S", "PH/S".
constexpr const char* kBitaxeUnitRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ/";

// Tail panels historically render at 160 px rather than the digit
// role's default 180 px — suffix glyphs ("H", "M") sit on a different
// baseline in Antonio and would clip the panel edge at 180. Shared
// with the OFFLINE banner and the best-diff tail.
constexpr float kBitaxeTailPx = 160.0f;

// Dot-inclusive ref for the hashrate digits — "1.2" shares a baseline
// with "527" at the same metric.
constexpr const char* kHashDigitRef = "0123456789.";

// Walk a UTF-8 string and return codepoints as one std::string each.
// Same contract as panel_texts.cpp's SplitUtf8Codepoints helper but
// kept local so this renderer isn't tied to that translation unit.
std::vector<std::string> SplitCodepoints(const std::string& s) {
  std::vector<std::string> out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((lead & 0xE0) == 0xC0) len = 2;
    else if ((lead & 0xF0) == 0xE0) len = 3;
    else if ((lead & 0xF8) == 0xF0) len = 4;
    if (i + len > s.size()) len = s.size() - i;
    out.emplace_back(s.substr(i, len));
    i += len;
  }
  return out;
}

// Right-align `value` codepoint-by-codepoint across `slots` cells with
// single-space padding on the left. Overlong values truncate leading
// codepoints so the least-significant characters stay visible. Mirrors
// the pre-refactor RenderBitaxeTail / RenderBitaxeHashrateTail padding
// so the hash parity checks match byte-for-byte.
std::vector<std::string> RightJustifyCodepoints(const std::string& value,
                                                std::size_t slots) {
  auto cells = SplitCodepoints(value);
  if (cells.size() < slots) {
    cells.insert(cells.begin(), slots - cells.size(), std::string(" "));
  } else if (cells.size() > slots) {
    cells.erase(cells.begin(), cells.begin() + (cells.size() - slots));
  }
  return cells;
}

// Build the panel-0 PaintSlot for the bitaxe logo. Always present
// (unlike mining-pool where some pools have no vendored logo) — the
// screen identity is unambiguous on the data screens that carry it.
PaintSlot BuildBitaxeLogoSlot() {
  PaintSlot slot{};
  slot.kind = PaintSlot::kIconBitmap;
  slot.bitmap = bitaxe_logo::kBitmap;
  slot.bmp_w = bitaxe_logo::kWidth;
  slot.bmp_h = bitaxe_logo::kHeight;
  return slot;
}

// Build a single-codepoint tail cell at the bitaxe-specific 160 px
// metric. Empty / ' ' strings short-circuit via kDigit's ' ' guard.
PaintSlot BuildTailCell(const std::string& cell, const char* ref) {
  PaintSlot slot{};
  slot.kind = PaintSlot::kDigit;
  slot.text = cell;
  slot.ref_override = ref;
  slot.pixel_height_override = kBitaxeTailPx;
  return slot;
}

}  // namespace

template <size_t N>
void RenderBitaxeHashrateScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& hostname,
    const std::optional<double>& hashrate_ghs,
    bool force_full,
    const std::string& prev_value) {
  static_assert(N >= 7, "bitaxe layout needs at least 7 panels");

  const bool offline = hostname.empty() || !hashrate_ghs;
  // `value` is the full "1.2TH" / "OFFLINE" string used for change
  // detection and the prev_value cache — keeps the caller's contract
  // unchanged (screen_manager.cpp stores this string between frames).
  const std::string value =
      offline ? std::string("OFFLINE")
              : FormatBitaxeHashrate(*hashrate_ghs);

  const bool full_refresh = force_full || prev_value.empty();
  // Bitaxe poll cadence is slow (~10 s) and the value rarely differs
  // by a single digit — any value-string change repaints every tail
  // panel without a per-codepoint diff.
  const bool tail_changed = full_refresh || value != prev_value;

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — bitaxe logo. Always paints on full refresh; kIconBitmap
  // on partial refresh with update=false is skipped entirely.
  slots[0] = BuildBitaxeLogoSlot();
  update[0] = full_refresh;

  if (offline) {
    // OFFLINE spans the whole tail (N-1 slots) — no split-text unit
    // cell when the device isn't reporting. Right-justify the 7-char
    // banner across N-1 slots so the 7-panel board fills exactly and
    // the 8-panel board leaves one leading blank.
    auto cells = RightJustifyCodepoints(value, N - 1);
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const std::size_t panel_idx = 1 + i;
      slots[panel_idx] = BuildTailCell(cells[i], kDigitRef);
      update[panel_idx] = tail_changed;
    }
  } else {
    // Success path: digits in N-2 slots at the 180 px digit metric + a
    // "<suffix>/S" split-text unit cell in slot N-1. The digit slots
    // share the dot-inclusive ref so "1.2" and "527" line up.
    const BitaxeHashrateParts parts = SplitBitaxeHashrate(*hashrate_ghs);
    auto cells = RightJustifyCodepoints(parts.value, N - 2);
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const std::size_t panel_idx = 1 + i;
      PaintSlot slot{};
      slot.kind = PaintSlot::kDigit;
      slot.text = cells[i];
      slot.ref_override = kHashDigitRef;
      slots[panel_idx] = slot;
      update[panel_idx] = tail_changed;
    }
    PaintSlot unit_slot{};
    unit_slot.kind = PaintSlot::kUnitSplit;
    unit_slot.text = parts.suffix + "/S";
    unit_slot.ref_override = kBitaxeUnitRef;
    slots[N - 1] = unit_slot;
    update[N - 1] = tail_changed;
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template <size_t N>
void RenderBitaxeBestDiffScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const std::string& hostname,
    const std::optional<std::string>& best_diff,
    bool force_full,
    const std::string& prev_value) {
  static_assert(N >= 7, "bitaxe layout needs at least 7 panels");

  const std::string value =
      (hostname.empty() || !best_diff || best_diff->empty())
          ? std::string("OFFLINE")
          : *best_diff;

  const bool full_refresh = force_full || prev_value.empty();
  const bool tail_changed = full_refresh || value != prev_value;

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — bitaxe logo (same single-panel layout as hashrate). Screen
  // identity is disambiguated by which value cycles in the tail slots.
  slots[0] = BuildBitaxeLogoSlot();
  update[0] = full_refresh;

  // Every tail panel renders through kDigit at the bitaxe-specific
  // 160 px metric — including the single-char suffix (M/G/T/P) which
  // sits on the digit baseline and would clip at 180 px.
  auto cells = RightJustifyCodepoints(value, N - 1);
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const std::size_t panel_idx = 1 + i;
    slots[panel_idx] = BuildTailCell(cells[i], kDigitRef);
    update[panel_idx] = tail_changed;
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderBitaxeHashrateScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&,
    const std::optional<double>&, bool, const std::string&);
template void RenderBitaxeHashrateScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&,
    const std::optional<double>&, bool, const std::string&);
template void RenderBitaxeBestDiffScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&,
    const std::optional<std::string>&, bool, const std::string&);
template void RenderBitaxeBestDiffScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&,
    const std::optional<std::string>&, bool, const std::string&);

}  // namespace btclock
