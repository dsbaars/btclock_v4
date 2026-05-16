// Nostr zap notification overlay. Transient — a zap receipt paints this
// for a few seconds, then the screen manager restores the slot that
// was on-screen before. Layout (7- and 8-panel):
//   [ZAP] [bolt] [ ] ... [ ] [sats glyph] [amount digits...]
// ZAP and the bolt are anchored to the leftmost two cells on every
// variant — V8's extra panel widens the blank gap between the bolt
// and the sats glyph rather than shifting the label run rightward.
// The bolt is the mdi::kIconLightningBolt glyph from the MDI icon
// font; the sats glyph and the amount digits sit at the right edge,
// with the sats glyph one slot before the most-significant amount
// digit. The amount is right-justified — short amounts ("1") leave
// the middle blank; longer ones ("1.2M") spill leftward through the
// blanks.

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "mdi_codepoints.hpp"
#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {

namespace {

// Ref chars for the "ZAP" label panel. Matches the uppercase + digit
// span every other text-label screen renders against, so the ZAP label
// row baseline visually aligns with MSCW / BTC/SUPPLY / BITAXE.
constexpr const char* kLabelRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Ref chars for the amount digits — includes the suffix letters so a
// "21K" / "1.2M" tail shares a baseline with a plain "100".
constexpr const char* kAmountRef = "0123456789kMB.";

// ZAP@0, bolt@1, [blanks 2..N-3], sats glyph just before the amount,
// amount right-justified ending at N-1. Same anchor positions on 7-
// and 8-panel boards — V8's extra cell becomes another blank between
// the bolt and the glyph, keeping the left-edge ZAP/bolt pair stable
// across variants. Zero-amount-cells output means the layout degraded
// to "no room for digits" — caller paints the amount at the rightmost
// panel and skips the glyph.
struct ZapLayout {
  std::size_t zap_slot;      // ZAP label.
  std::size_t bolt_slot;     // MDI lightning-bolt icon.
  std::size_t glyph_slot;    // Sats glyph (one before first amount).
  std::size_t first_amount;  // Leftmost amount-digit slot.
  std::size_t amount_cells;  // Count of amount cells (= N - first_amount).
};

template <std::size_t N>
ZapLayout ComputeZapLayout(std::size_t amount_chars, bool reserve_glyph_cell) {
  ZapLayout L{};
  L.zap_slot = 0;
  L.bolt_slot = L.zap_slot + 1;
  // Available tail = panels after [ZAP][bolt]. Amount is right-justified
  // into it; the sats glyph wants a slot just before the most-significant
  // digit. When the amount run consumes the entire tail the glyph drops
  // (the integer-rendering path can fill the tail; surrendering a digit
  // for the glyph would defeat the "show full amount when it fits" rule).
  const std::size_t available_tail = N - (L.bolt_slot + 1);
  std::size_t amount = amount_chars;
  if (amount > available_tail) amount = available_tail;
  if (amount < 1) amount = 1;
  L.first_amount = N - amount;
  L.amount_cells = amount;
  L.glyph_slot = (reserve_glyph_cell && L.first_amount > L.bolt_slot + 1)
                     ? L.first_amount - 1
                     : N;  // sentinel "no glyph slot"
  return L;
}

}  // namespace

template <size_t N>
void RenderNostrZapScreen(
    std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const DataSnapshot::LatestZap& zap, bool use_sats_symbol,
    bool use_btc_symbol, uint8_t sats_variant, bool full_refresh_mode,
    bool vertical_desc) {
  static_assert(N >= 7, "Zap layout needs at least 7 panels");

  // Budget = tail cells excluding ZAP + bolt. The glyph isn't reserved
  // here on purpose: when the integer fills the tail the layout step
  // drops the glyph rather than truncating the amount.
  constexpr std::size_t kAmountBudget = N - 2;
  const std::string amount = FormatZapAmount(zap.amount_sats, kAmountBudget);
  const bool reserve_glyph = use_sats_symbol || use_btc_symbol;
  const ZapLayout L = ComputeZapLayout<N>(amount.size(), reserve_glyph);

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Default every panel to blank. The zap overlay is always painted as
  // a single full frame (the screen manager restores the prior screen
  // a few seconds later), so every cell is flagged for repaint.
  for (std::size_t i = 0; i < N; ++i) {
    slots[i].kind = PaintSlot::kBlank;
    update[i] = true;
  }

  // ZAP label cell.
  PaintSlot zap_slot{};
  zap_slot.kind = PaintSlot::kLabel;
  zap_slot.text = "ZAP";
  zap_slot.ref_override = kLabelRef;
  slots[L.zap_slot] = zap_slot;

  // Lightning-bolt MDI glyph.
  PaintSlot bolt_slot{};
  bolt_slot.kind = PaintSlot::kMdiIcon;
  bolt_slot.mdi_codepoint = mdi::kIconLightningBolt;
  slots[L.bolt_slot] = bolt_slot;

  // Sats glyph cell (when enabled and there's room for it). The renderer
  // paints the same kSatsGlyph slot the Moscow-time screen uses so the
  // glyph weight matches across screens.
  if (L.glyph_slot < N) {
    PaintSlot g{};
    if (use_btc_symbol) {
      g.kind = PaintSlot::kBtcGlyph;
      g.text = kBtcSignUtf8;
    } else {
      const auto glyph = SatsGlyphUtf8(sats_variant);
      g.kind = PaintSlot::kSatsGlyph;
      g.text = glyph.c_str();
    }
    slots[L.glyph_slot] = g;
  }

  // Amount cells, right-justified. amount.size() <= L.amount_cells by
  // construction (ComputeZapLayout grows the run to fit); leading cells
  // get blanks when the amount is shorter than the run.
  const std::size_t pad =
      (L.amount_cells > amount.size()) ? L.amount_cells - amount.size() : 0;
  for (std::size_t i = 0; i < L.amount_cells; ++i) {
    const std::size_t panel_idx = L.first_amount + i;
    if (i < pad) {
      slots[panel_idx].kind = PaintSlot::kBlank;
      continue;
    }
    const char ch = amount[i - pad];
    if (ch == ' ') {
      slots[panel_idx].kind = PaintSlot::kBlank;
      continue;
    }
    PaintSlot s{};
    s.kind = PaintSlot::kDigit;
    s.text.assign(1, ch);
    s.ref_override = kAmountRef;
    slots[panel_idx] = s;
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update,
                  /*full_refresh=*/full_refresh_mode, vertical_desc);
}

template void RenderNostrZapScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&, bool, bool, uint8_t, bool,
    bool);
template void RenderNostrZapScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&, bool, bool, uint8_t, bool,
    bool);

}  // namespace btclock
