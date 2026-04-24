#include "screens/screens.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "screens/assets/bitaxe_logo.hpp"
#include "screens/common.hpp"
#include "screens/panel_texts.hpp"

namespace btclock {
namespace {

// Split-text unit sizing — matches mining_pool.cpp's kUnitSplitPx so
// "GH/S" / "TH/S" / "PH/S" reads as a unit label next to the digits.
constexpr float kBitaxeUnitSplitPx = 54.0f;
// Ref chars for the unit split-text. Uppercase + '/' + 'S' covers
// "GH/S", "TH/S", "PH/S".
constexpr const char* kBitaxeUnitRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ/";

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

// Paint the vendored 1-bpp MSB-first bitaxe logo centred on `lfb`. Mirrors
// mining_pool.cpp's PaintInvertedBitmap semantics: a 0 bit in the source
// means ink (black), a 1 bit means leave the background alone. Caller is
// responsible for ClearFb() before this. The logo is 88×220; panels are
// 128×296 in landscape (Rev B 2.9") / 122×250 (Rev A 2.13") so the bitmap
// sits within the panel and the out-of-bounds guard here is purely defensive.
void PaintBitaxeLogo(LandscapeFb& lfb) {
  const int panel_w = LogicalWidth(lfb);
  const int panel_h = LogicalHeight(lfb);
  const int bmp_w = bitaxe_logo::kWidth;
  const int bmp_h = bitaxe_logo::kHeight;
  const int x_off = (panel_w - bmp_w) / 2;
  const int y_off = (panel_h - bmp_h) / 2;
  const int stride = (bmp_w + 7) / 8;
  for (int py = 0; py < bmp_h; ++py) {
    const std::uint8_t* row = bitaxe_logo::kBitmap + py * stride;
    for (int px = 0; px < bmp_w; ++px) {
      const std::uint8_t byte = row[px >> 3];
      const std::uint8_t bit =
          static_cast<std::uint8_t>(1U << (7 - (px & 7)));
      if ((byte & bit) == 0) {
        SetPixelLandscape(lfb, x_off + px, y_off + py, /*white=*/false);
      }
    }
  }
}

// Right-align `value` across the tail panels, one codepoint per panel.
// Short values get blank cells on the left; overlong values truncate
// leading cells so the least-significant characters stay visible. Panel
// 0 carries the bitaxe logo bitmap, so the tail starts at panel 1 and
// spans N-1 slots (one more than the old two-panel-label layout).
template <size_t N>
void RenderBitaxeTail(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                      uint8_t (&fb_storage)[N][16 * 296],
                      const AppFonts& fonts,
                      const std::string& value) {
  constexpr size_t kTail = N - 1;
  auto cells = SplitCodepoints(value);
  if (cells.size() < kTail) {
    cells.insert(cells.begin(), kTail - cells.size(), std::string(" "));
  } else if (cells.size() > kTail) {
    cells.erase(cells.begin(), cells.begin() + (cells.size() - kTail));
  }
  for (size_t i = 0; i < kTail; ++i) {
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    const std::string& c = cells[i];
    if (c.empty() || c == " ") continue;
    // Shrink the ink size a touch so suffix glyphs ("H", "M") that sit
    // on different baselines in Antonio don't clip the panel edge —
    // digits/letters fit at 180 pt the same way block-height draws.
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                     c.c_str(), kDigitRef, fonts.antonio(), 160.0f,
                     /*white_text=*/false);
  }
}

// Hashrate-specific tail: slots 1..N-2 carry the digit value (one
// codepoint per panel, right-justified), slot N-1 carries the
// "<suffix>/S" split-text unit cell (top half "<suffix>", bottom half
// "S", separated by the 6 px pill line — same visual language as
// mining-pool "PH/S" / "GH/S"). Frees one more digit slot compared to
// the old "G" "H" two-panel unit layout.
template <size_t N>
void RenderBitaxeHashrateTail(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& digits, const std::string& suffix) {
  constexpr size_t kDigitSlots = N - 2;
  auto cells = SplitCodepoints(digits);
  if (cells.size() < kDigitSlots) {
    cells.insert(cells.begin(), kDigitSlots - cells.size(), std::string(" "));
  } else if (cells.size() > kDigitSlots) {
    cells.erase(cells.begin(),
                cells.begin() + (cells.size() - kDigitSlots));
  }
  for (size_t i = 0; i < kDigitSlots; ++i) {
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    const std::string& c = cells[i];
    if (c.empty() || c == " ") continue;
    // Digits include '.' for sub-10 TH/PH rendering — use the dot-
    // inclusive ref so "1.2" shares a baseline with "527".
    static constexpr const char* kHashDigitRef = "0123456789.";
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, c.c_str(),
                     kHashDigitRef, fonts.antonio(), 180.0f,
                     /*white_text=*/false);
  }
  auto unit = PrepFb(panels, fb_storage, N - 1);
  ClearFb(unit, /*white=*/true);
  DrawSplitText(unit, unit.native_width, unit.native_height,
                suffix.c_str(), "S", kBitaxeUnitRef, fonts.antonio(),
                kBitaxeUnitSplitPx, /*white_text=*/false);
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

  if (full_refresh) {
    // Bitaxe logo occupies panel 0 only. The old firmware's icons.cpp
    // bitmap ships inline (see main/screens/assets/bitaxe_logo.cpp) so
    // we don't need a hasLogo/text-fallback branch — there's always a
    // graphical identity cell for the bitaxe screens.
    auto lfb0 = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb0, /*white=*/true);
    PaintBitaxeLogo(lfb0);
  }

  // For simplicity, any value-string change repaints every tail panel —
  // the bitaxe poll cadence is slow (~10 s) and the value rarely differs
  // by a single digit, so a per-codepoint diff would add code without
  // buying visible refresh improvement.
  const bool tail_changed = full_refresh || value != prev_value;
  if (tail_changed) {
    if (offline) {
      // OFFLINE spans the whole tail (N-1 slots) — no split-text unit
      // cell when the device isn't reporting.
      RenderBitaxeTail(panels, fb_storage, fonts, value);
    } else {
      // Success path: digits in N-2 slots + "<suffix>/S" split-text
      // unit cell in slot N-1.
      const BitaxeHashrateParts parts = SplitBitaxeHashrate(*hashrate_ghs);
      RenderBitaxeHashrateTail(panels, fb_storage, fonts, parts.value,
                               parts.suffix);
    }
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) {
    panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  }
  if (tail_changed) {
    for (size_t i = 1; i < N; ++i) {
      panels[i]->DrawFramebufferStart(fb_storage[i], kind);
    }
  }
  if (full_refresh) {
    panels[0]->WaitForRefresh();
  }
  if (tail_changed) {
    for (size_t i = 1; i < N; ++i) panels[i]->WaitForRefresh();
  }
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

  if (full_refresh) {
    // Same single-panel logo layout as the hashrate screen — the screen
    // identity is disambiguated by which value cycles in the tail slots.
    auto lfb0 = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb0, /*white=*/true);
    PaintBitaxeLogo(lfb0);
  }

  const bool tail_changed = full_refresh || value != prev_value;
  if (tail_changed) {
    RenderBitaxeTail(panels, fb_storage, fonts, value);
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) {
    panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  }
  if (tail_changed) {
    for (size_t i = 1; i < N; ++i) {
      panels[i]->DrawFramebufferStart(fb_storage[i], kind);
    }
  }
  if (full_refresh) {
    panels[0]->WaitForRefresh();
  }
  if (tail_changed) {
    for (size_t i = 1; i < N; ++i) panels[i]->WaitForRefresh();
  }
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
