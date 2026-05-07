#include <array>
#include <cstddef>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {
namespace {

// Reference-character set for centering. Includes upper-case A..Z plus
// digits so labels ("BLOCK", "HEIGHT") and single-char digits align on
// the same baseline — matches the old firmware where showChars and
// showDigit paths both sat on the digit-height baseline.
constexpr const char* kAnyRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Pick a render size scaling roughly with the cell length:
// - 1 char  → large (digit-sized).
// - 2 chars → medium (old firmware routed length==2 to fontBig; we use
//             the same Antonio glyph at a slightly smaller size to fit
//             two digits side-by-side without clipping).
// - >=3 chars → auto-fit, bounded so single-word labels still read.
float PickPixelHeight(const char* text, int panel_w, const Font& font) {
  // Cheap single-byte length — enough for ASCII. UTF-8 tokens wider than
  // their byte count still FitTextPx-clamp correctly below.
  int n = 0;
  while (text[n] != '\0' && n < 64) ++n;
  if (n == 0) return 0.0f;
  if (n == 1) return FitTextPx(text, font, 120.0f, 20.0f, panel_w - 4);
  if (n == 2) return FitTextPx(text, font, 90.0f, 18.0f, panel_w - 4);
  // Longer strings — fall back to fit-to-width; don't go below ~14 px
  // or the result becomes unreadable at normal viewing distance.
  return FitTextPx(text, font, 64.0f, 14.0f, panel_w - 6);
}

template <size_t N>
void PaintOne(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
              uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
              size_t i, const std::string& cell) {
  auto lfb = PrepFb(panels, fb_storage, i);
  ClearFb(lfb, /*white=*/true);
  if (cell.empty()) return;

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

  const Font& font = fonts.digit();
  const float px = PickPixelHeight(cell.c_str(), lfb.native_width, font);
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
                        bool cell_diff_reset, bool full_refresh_mode) {
  // `cell_diff_reset` forces every cell to repaint (transition / first
  // paint). `full_refresh_mode` drives the EPD refresh kind.
  std::array<bool, N> dirty{};
  for (size_t i = 0; i < N; ++i) {
    dirty[i] =
        cell_diff_reset || full_refresh_mode || cells[i] != prev_cells[i];
  }

  for (size_t i = 0; i < N; ++i) {
    if (!dirty[i]) continue;
    PaintOne(panels, fb_storage, fonts, i, cells[i]);
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
    const std::array<std::string, 7>&, bool, bool);
template void RenderCustomScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::array<std::string, 8>&,
    const std::array<std::string, 8>&, bool, bool);

}  // namespace btclock
