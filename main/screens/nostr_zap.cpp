// Nostr zap notification overlay. Transient — a zap receipt paints this
// for a few seconds, then the screen manager restores the slot that
// was on-screen before. Layout: "ZAP" text label on panel 0, scaled
// amount (e.g. "21k") right-justified across the tail panels.

#include "screens/screens.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "screens/common.hpp"

namespace btclock {
namespace {

// "0123456789kMBA" keeps scaled amounts ("21k", "1.2M") vertically
// aligned with the label's typographic baseline. Expanding the ref
// past what any of the cells use would push the baseline down and
// leave the glyph floating.
constexpr const char* kZapRef = "0123456789kMBA";

// Reference charset for the "ZAP" label panel. Shares the uppercase +
// digit span used by moscow_time / bitcoin_supply / bitaxe so the
// label row baseline matches every other text-label screen.
constexpr const char* kLabelRef =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Amount starts immediately after the "ZAP" label on slot 0. The old
// icon + blank layout freed panel 1 for data, so the scaled amount
// spreads across every remaining cell.
constexpr std::size_t kLabelSlot = 0;
constexpr std::size_t kAmountStart = 1;

}  // namespace

std::string FormatZapAmount(const std::optional<int64_t>& amount_sats) {
  if (!amount_sats || *amount_sats < 0) return "?";
  const int64_t v = *amount_sats;
  // Small values render as plain integers so "21 sats" shows "21",
  // not "0.0k" — matches old-firmware parseZapNotify's raw-digit path
  // for amounts under 1000.
  if (v < 1000) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return buf;
  }
  // Pick the largest suffix (k / M / B) whose integer part has at most
  // 3 digits. Fractional digit drops once the integer hits 3 digits so
  // we don't overflow the 4-char cell budget ("1.2k" / "12k" / "123k").
  double x = static_cast<double>(v);
  const char* suffix;
  if (v >= 1'000'000'000LL) { x /= 1e9; suffix = "B"; }
  else if (v >= 1'000'000LL) { x /= 1e6; suffix = "M"; }
  else { x /= 1e3; suffix = "k"; }
  char buf[16];
  if (x >= 100.0) {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(x + 0.5), suffix);
  } else if (x >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(x + 0.5), suffix);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f%s", x, suffix);
  }
  return buf;
}

namespace {

template <size_t N>
void PaintBlank(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                uint8_t (&fb_storage)[N][16 * 296], std::size_t i) {
  auto lfb = PrepFb(panels, fb_storage, i);
  ClearFb(lfb, /*white=*/true);
}

template <size_t N>
void PaintCentered(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                   uint8_t (&fb_storage)[N][16 * 296],
                   std::size_t i, const char* text,
                   const Font& font, float max_px, float min_px,
                   const char* ref_chars) {
  auto lfb = PrepFb(panels, fb_storage, i);
  ClearFb(lfb, /*white=*/true);
  if (text == nullptr || text[0] == '\0') return;
  const float px = FitTextPx(text, font, max_px, min_px,
                             lfb.native_width - 6);
  DrawTextCentered(lfb, lfb.native_width, lfb.native_height, text,
                   ref_chars, font, px, /*white_text=*/false);
}

}  // namespace

template <size_t N>
void RenderNostrZapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DataSnapshot::LatestZap& zap) {
  // "ZAP" label on panel 0 — same typography as MSCW / BTC/SUPPLY so
  // the notification reads as part of the same visual family, not as a
  // one-off overlay. FitTextPx shrinks only if the word wouldn't fit;
  // at 140 px cap "ZAP" sits comfortably within panel width.
  PaintCentered(panels, fb_storage, kLabelSlot, "ZAP",
                fonts.antonio(), /*max_px=*/140.0f, /*min_px=*/40.0f,
                /*ref_chars=*/kLabelRef);

  // Amount spreads across every tail cell — no message snippet, no
  // icon slot. On N=7 that's 6 cells for a max-4-char scaled string
  // ("1.2M"), leaving ≥ 2 leading blanks.
  const std::string amount = FormatZapAmount(zap.amount_sats);
  const std::size_t amount_cells = (N > kAmountStart) ? N - kAmountStart : 0;

  // Paint amount right-justified. If the scaled string is shorter than
  // the slot count, pad with leading blanks; if longer (shouldn't happen
  // since FormatZapAmount clamps to 4 chars), truncate leading chars.
  for (std::size_t i = 0; i < amount_cells; ++i) {
    const std::size_t slot = kAmountStart + i;
    char glyph = '\0';
    if (amount.size() >= amount_cells) {
      glyph = amount[amount.size() - amount_cells + i];
    } else {
      const std::size_t pad = amount_cells - amount.size();
      if (i >= pad) glyph = amount[i - pad];
    }
    if (glyph == '\0' || glyph == ' ') {
      PaintBlank(panels, fb_storage, slot);
    } else {
      const char one[2] = {glyph, '\0'};
      PaintCentered(panels, fb_storage, slot, one, fonts.antonio(),
                    /*max_px=*/110.0f, /*min_px=*/24.0f,
                    /*ref_chars=*/kZapRef);
    }
  }

  // Full refresh — single-shot notification, no value in partial.
  for (std::size_t i = 0; i < N; ++i) {
    panels[i]->DrawFramebufferStart(fb_storage[i], RefreshKind::kFull);
  }
  for (std::size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

template void RenderNostrZapScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&);
template void RenderNostrZapScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&);

}  // namespace btclock
