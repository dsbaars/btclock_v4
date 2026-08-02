#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mdi_custom_cell.hpp"
#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {
namespace {

// True when `cell` is exactly one sats-symbol PUA codepoint
// (U+E000..U+E00F). The digit font has no glyph in that range, so
// without this check PaintOne draws tofu for a caller that POSTs the
// sats symbol to /api/show/custom. Route those cells to the dedicated
// sats font instead.
bool IsSatsGlyphCell(const std::string& cell) {
  if (cell.size() != 3) return false;
  const auto b0 = static_cast<unsigned char>(cell[0]);
  const auto b1 = static_cast<unsigned char>(cell[1]);
  const auto b2 = static_cast<unsigned char>(cell[2]);
  if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
  const std::uint32_t cp = (static_cast<std::uint32_t>(b0 & 0x0F) << 12) |
                           (static_cast<std::uint32_t>(b1 & 0x3F) << 6) |
                           (static_cast<std::uint32_t>(b2 & 0x3F));
  return cp >= 0xE000u && cp <= 0xE00Fu;
}

// The moscow renderer pairs a 130 px sats glyph with 180 px digits
// (common.cpp kSatsGlyphPx / kDigitPxDefault). Preserve that ratio here
// so a pushed sats symbol weight-matches the digits beside it instead of
// towering over them.
constexpr float kSatsGlyphToDigitRatio = 130.0f / 180.0f;

// Reference-character set for centering. Includes upper-case A..Z plus
// digits so labels ("BLOCK", "HEIGHT") and single-char digits align on
// the same baseline — matches the old firmware where showChars and
// showDigit paths both sat on the digit-height baseline.
constexpr const char* kAnyRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Pick a render size scaling roughly with the cell's *codepoint*
// length:
// - 1 char  → large (digit-sized).
// - 2 chars → medium (old firmware routed length==2 to fontBig; we use
//             the same Antonio glyph at a slightly smaller size to fit
//             two digits side-by-side without clipping).
// - >=3 chars → auto-fit, bounded so single-word labels still read.
// `digit_px > 0` raises the ceiling for the short (1-2 codepoint) cells
// so a caller can render digits at a chosen size instead of the 120/90 px
// caps. Applied as the FitTextPx *max*, so a glyph that still wouldn't fit
// the panel shrinks rather than clipping. Multi-char labels (n>=3) ignore
// it — spreading a label at digit size makes no sense.
float PickPixelHeight(const char* text, int panel_w, const Font& font,
                      float digit_px) {
  // Count UTF-8 codepoints, NOT bytes — a single non-ASCII glyph like
  // ₿ (U+20BF, 3 bytes in UTF-8) would otherwise route to the >=3
  // branch and render at half the digit-cell size. UTF-8 leading bytes
  // have either bit pattern 0xxxxxxx (ASCII) or 11xxxxxx (start of
  // multi-byte sequence); continuation bytes are 10xxxxxx and we skip
  // those when counting codepoints.
  int n = 0;
  for (const char* p = text; *p != '\0' && n < 64; ++p) {
    if ((static_cast<uint8_t>(*p) & 0xC0) != 0x80) ++n;
  }
  if (n == 0) return 0.0f;
  const bool over = digit_px > 0.0f;
  if (n == 1)
    return FitTextPx(text, font, over ? digit_px : 120.0f, 20.0f, panel_w - 4);
  if (n == 2)
    return FitTextPx(text, font, over ? digit_px : 90.0f, 18.0f, panel_w - 4);
  // Longer strings — fall back to fit-to-width; don't go below ~14 px
  // or the result becomes unreadable at normal viewing distance.
  return FitTextPx(text, font, 64.0f, 14.0f, panel_w - 6);
}

template <size_t N>
void PaintOne(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
              uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
              size_t i, const std::string& cell, float digit_px) {
  auto lfb = PrepFb(panels, fb_storage, i);
  ClearFb(lfb, /*white=*/true);
  if (cell.empty()) return;

  std::uint32_t mdi_cp = 0;
  if (ParseCustomCellMdi(cell, &mdi_cp)) {
    if (mdi_cp != 0) {
      DrawCodepointCentered(lfb, lfb.native_width, lfb.native_height, mdi_cp,
                            fonts.icon(), kMdiCustomCellPixelPx,
                            /*white_text=*/false);
    }
    return;
  }

  // Split-text path: a single '/' interior separator gets top/bottom
  // layout, matching the old EPDManager::splitText dispatch for labels
  // like "BLOCK/HEIGHT" and "FEE/RATE". Leading or trailing '/' falls
  // through to the single-line path — rendering "/FOO" as a top half of
  // "" and bottom "FOO" would look broken.
  const auto slash = cell.find('/');
  if (slash != std::string::npos && slash > 0 && slash + 1 < cell.size() &&
      cell.find('/', slash + 1) == std::string::npos) {
    std::string top = cell.substr(0, slash);
    std::string bot = cell.substr(slash + 1);
    // Fit the wider half so both halves share a single pixel height —
    // keeps letter metrics consistent across the two rows.
    const int target = lfb.native_width - 6;
    float px_top =
        FitTextPx(top.c_str(), fonts.oswald_bold(), 54.0f, 14.0f, target);
    float px_bot =
        FitTextPx(bot.c_str(), fonts.oswald_bold(), 54.0f, 14.0f, target);
    const float px = px_top < px_bot ? px_top : px_bot;
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, top.c_str(),
                  bot.c_str(), kAnyRef, fonts.oswald_bold(), px,
                  /*white_text=*/false);
    return;
  }

  // Sats-symbol glyph — a single PUA codepoint via the dedicated sats
  // font. Sized like a single-codepoint digit cell (PickPixelHeight's
  // n==1 branch) but weight-matched to the digits via the 130/180 ratio.
  // ref_chars is the glyph itself so centering uses its own bbox —
  // kAnyRef would compute a zero ref box for a PUA codepoint.
  if (IsSatsGlyphCell(cell)) {
    const float cap = (digit_px > 0.0f) ? digit_px : 120.0f;
    const float sats_px =
        FitTextPx(cell.c_str(), fonts.sats_glyph(),
                  cap * kSatsGlyphToDigitRatio, 20.0f, lfb.native_width - 4);
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, cell.c_str(),
                     cell.c_str(), fonts.sats_glyph(), sats_px,
                     /*white_text=*/false);
    return;
  }

  const Font& font = fonts.digit();
  const float px =
      PickPixelHeight(cell.c_str(), lfb.native_width, font, digit_px);
  DrawTextCentered(lfb, lfb.native_width, lfb.native_height, cell.c_str(),
                   kAnyRef, font, px, /*white_text=*/false);
}

}  // namespace

template <size_t N>
void RenderCustomScreen(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
                        uint8_t (&fb_storage)[N][16 * 296],
                        const AppFonts& fonts,
                        const std::array<std::string, N>& cells,
                        const std::array<std::string, N>& prev_cells,
                        bool cell_diff_reset, bool full_refresh_mode,
                        float digit_px) {
  // `cell_diff_reset` forces every cell to repaint (transition / first
  // paint). `full_refresh_mode` drives the EPD refresh kind.
  std::array<bool, N> dirty{};
  for (size_t i = 0; i < N; ++i) {
    dirty[i] =
        cell_diff_reset || full_refresh_mode || cells[i] != prev_cells[i];
  }

  for (size_t i = 0; i < N; ++i) {
    if (!dirty[i]) continue;
    PaintOne(panels, fb_storage, fonts, i, cells[i], digit_px);
  }

  const RefreshKind kind =
      full_refresh_mode ? RefreshKind::kFull : RefreshKind::kPartial;
  for (size_t i = 0; i < N; ++i) {
    if (!dirty[i]) continue;
    panels[i]->DrawFramebufferStart(fb_storage[i], kind);
  }
  for (size_t i = 0; i < N; ++i) {
    if (!dirty[i]) continue;
    panels[i]->WaitForRefresh();
  }
}

template void RenderCustomScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::array<std::string, 7>&,
    const std::array<std::string, 7>&, bool, bool, float);
template void RenderCustomScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::array<std::string, 8>&,
    const std::array<std::string, 8>&, bool, bool, float);

}  // namespace btclock
