#include "screens/screens.hpp"

#include <cstring>
#include <string>

#include "mdi_codepoints.hpp"
#include "screens/assets/pool_logos.hpp"
#include "screens/common.hpp"
#include "screens/panel_texts.hpp"

namespace btclock {

// Reference chars for the split-text pool label. Uppercase + digits
// covers every display name the pool plugins register today; narrow the
// set would drop a baseline on names like "Ocean" vs "OCEAN".
namespace {
constexpr const char* kPoolLabelRef =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
// Unit label sizing — small enough that "PH/S" / "SATS" / "BTC" fits
// the panel width but large enough to read at arm's length. Matches the
// fee-rate trailing unit panel visually (split-text at 54 px).
constexpr float kUnitSplitPx = 54.0f;
// Pool-name fallback sizing. Matches the unit panel so the two text
// panels read as a pair when the pool has no vendored logo (e.g.
// "SATOSHI" / "RADIO"). Antonio at 54 px comfortably fits 8–9 caps on
// the 250 px-wide panel.
constexpr float kPoolNamePx = 54.0f;

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

// Paint a 1-bpp MSB-first bitmap centred on the landscape framebuffer.
// Matches the semantics of the old firmware's
// `display.drawInvertedBitmap`: a 0 bit in the source = ink (black),
// a 1 bit = background (no draw). Caller is expected to ClearFb() first.
// Out-of-bounds pixels are silently clipped — bitmaps larger than the
// panel would be drawn partially, which is correct for a letterboxed
// layout and avoids a defensive assert on device.
void PaintInvertedBitmap(LandscapeFb& lfb, const std::uint8_t* bitmap,
                         int bmp_w, int bmp_h) {
  const int panel_w = LogicalWidth(lfb);
  const int panel_h = LogicalHeight(lfb);
  const int x_off = (panel_w - bmp_w) / 2;
  const int y_off = (panel_h - bmp_h) / 2;
  const int stride = (bmp_w + 7) / 8;
  for (int py = 0; py < bmp_h; ++py) {
    const std::uint8_t* row = bitmap + py * stride;
    for (int px = 0; px < bmp_w; ++px) {
      const std::uint8_t byte = row[px >> 3];
      const std::uint8_t bit =
          static_cast<std::uint8_t>(1U << (7 - (px & 7)));
      // A 0 bit means "draw ink" in the v3 bitmap format. Skip 1s so
      // the ClearFb(white) background shows through.
      if ((byte & bit) == 0) {
        SetPixelLandscape(lfb, x_off + px, y_off + py, /*white=*/false);
      }
    }
  }
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

// Paint the single label panel (panel 0) with either the vendored bitmap
// logo or the split-text pool-name fallback. Panel 1 is no longer part of
// the label area (v3 convention restored): freeing it for a digit cell
// widens the earnings formatter's usable width from 4 to 5 slots, which
// fixes the "50.0K" → "0.0K" left-truncation on 7-panel boards.
//
// For split-text names ("Satoshi Radio", "gobrrr_pool", "public_pool"):
// uses DrawSplitText, which draws a horizontal separator line between
// the two halves — visually pairs with the other slash-labelled panels
// (BLOCK/HEIGHT, FEE/RATE etc.). Single-word names (no delimiter) render
// as a single centred string without the separator line.
template <size_t N>
void PaintPoolLabelPanels(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                          uint8_t (&fb_storage)[N][16 * 296],
                          const AppFonts& fonts, const std::string& name) {
  static_assert(N >= 3,
                "mining-pool label layout needs at least 3 panels "
                "(1 label + >=1 digit + 1 trailing unit)");

  auto panel0 = PrepFb(panels, fb_storage, 0);
  ClearFb(panel0, /*white=*/true);

  // Logo path. Every vendored logo fits within a single panel (122×122
  // for gobrrr/noderunners/ocean; 37×230 for braiins) — matches v3's
  // single-panel logo convention.
  if (const pool_logos::PoolLogo* logo = pool_logos::Lookup(name)) {
    PaintInvertedBitmap(panel0, logo->bitmap, logo->width, logo->height);
    return;
  }

  if (name.empty()) return;

  const PoolLabelSplit split = SplitPoolName(name);
  if (split.bottom.empty()) {
    // Single-word pool name: paint as one centred string, no separator.
    DrawTextCentered(panel0, panel0.native_width, panel0.native_height,
                     split.top.c_str(), kPoolLabelRef, fonts.antonio(),
                     kPoolNamePx, /*white_text=*/false);
    return;
  }
  // Two halves with separator line — same visual shape as BLOCK/HEIGHT
  // and FEE/RATE. DrawSplitText centres each half on its own sub-panel
  // and draws the 6 px pill-ended line between them.
  DrawSplitText(panel0, panel0.native_width, panel0.native_height,
                split.top.c_str(), split.bottom.c_str(), kPoolLabelRef,
                fonts.antonio(), kPoolNamePx, /*white_text=*/false);
}

// Paint the unit label on the trailing panel at split-text scale so it
// reads as a label rather than a digit. Single-token (e.g. "PH/S" gets
// split on '/' if present; otherwise plain centred text).
template <size_t N>
void PaintUnitPanel(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                    uint8_t (&fb_storage)[N][16 * 296],
                    const AppFonts& fonts, const std::string& unit) {
  auto lfb = PrepFb(panels, fb_storage, N - 1);
  ClearFb(lfb, /*white=*/true);
  if (unit.empty()) return;
  // "PH/S" style unit splits on the '/' into top/bottom halves, keeping
  // the visual pair with the FEE/RATE-style label screens; "SATS" /
  // "BTC" render as a single-line centred string.
  const auto slash = unit.find('/');
  if (slash != std::string::npos) {
    const std::string top = unit.substr(0, slash);
    const std::string bot = unit.substr(slash + 1);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, top.c_str(),
                  bot.c_str(), kPoolLabelRef, fonts.antonio(), kUnitSplitPx,
                  /*white_text=*/false);
  } else {
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, unit.c_str(),
                     kPoolLabelRef, fonts.antonio(), kUnitSplitPx,
                     /*white_text=*/false);
  }
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

  if (full_refresh) {
    PaintPoolLabelPanels(panels, fb_storage, fonts, pool.name);
  }

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

  std::array<bool, kDigitPanels> update{};
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh ||
                (i < prev_digits.size()
                     ? now_digits[i] != prev_digits[i]
                     : now_digits[i] != ' ');
  }

  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, kFirstDigitPanel + i);
    ClearFb(lfb, /*white=*/true);
    const char c = now_digits[i];
    if (c == ' ') continue;
    const char one[2] = {c, '\0'};
    // Dot-inclusive ref so "1.3" and "123" share the same baseline — the
    // hashrate value can include a decimal point depending on magnitude.
    static constexpr const char* kHashDigitRef = "0123456789.";
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                     kHashDigitRef, fonts.antonio(), 180.0f,
                     /*white_text=*/false);
  }

  if (full_refresh || now_layout.unit != prev_layout.unit) {
    PaintUnitPanel(panels, fb_storage, fonts, now_layout.unit);
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) {
    // Only panel 0 carries the label area now — panel 1 is a digit cell
    // whose refresh is driven by the update[] mask below.
    panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  }
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[kFirstDigitPanel + i]->DrawFramebufferStart(
        fb_storage[kFirstDigitPanel + i], kind);
  }
  if (full_refresh || now_layout.unit != prev_layout.unit) {
    panels[N - 1]->DrawFramebufferStart(fb_storage[N - 1], kind);
  }
  if (full_refresh) {
    panels[0]->WaitForRefresh();
  }
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[kFirstDigitPanel + i]->WaitForRefresh();
  }
  if (full_refresh || now_layout.unit != prev_layout.unit) {
    panels[N - 1]->WaitForRefresh();
  }
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

  if (full_refresh) {
    PaintPoolLabelPanels(panels, fb_storage, fonts, pool.name);
  }

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

  std::array<bool, kDigitPanels> update{};
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh ||
                (i < prev_digits.size()
                     ? now_digits[i] != prev_digits[i]
                     : now_digits[i] != ' ');
  }

  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, kFirstDigitPanel + i);
    ClearFb(lfb, /*white=*/true);
    const char c = now_digits[i];
    if (c == ' ') continue;
    const char one[2] = {c, '\0'};
    // "1.5M" / "12.3K" / "500" share the digit+'.'+letter ref. The
    // K/M/BTC suffix characters share Antonio's baseline with the digits.
    static constexpr const char* kEarnDigitRef = "0123456789.KMB";
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                     kEarnDigitRef, fonts.antonio(), 180.0f,
                     /*white_text=*/false);
  }

  // Unit label: "SATS" by default; "BTC" when the whale-mode branch
  // fires in LayoutMiningPoolEarnings. Invalid data still paints "SATS"
  // so users see an expected unit on the trailing panel.
  const std::string unit =
      now_layout.valid ? now_layout.unit_label : std::string("SATS");
  const std::string prev_unit =
      prev_layout.valid ? prev_layout.unit_label : std::string("SATS");
  if (full_refresh || unit != prev_unit) {
    PaintUnitPanel(panels, fb_storage, fonts, unit);
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) {
    panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  }
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[kFirstDigitPanel + i]->DrawFramebufferStart(
        fb_storage[kFirstDigitPanel + i], kind);
  }
  if (full_refresh || unit != prev_unit) {
    panels[N - 1]->DrawFramebufferStart(fb_storage[N - 1], kind);
  }
  if (full_refresh) {
    panels[0]->WaitForRefresh();
  }
  for (std::size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[kFirstDigitPanel + i]->WaitForRefresh();
  }
  if (full_refresh || unit != prev_unit) {
    panels[N - 1]->WaitForRefresh();
  }
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
