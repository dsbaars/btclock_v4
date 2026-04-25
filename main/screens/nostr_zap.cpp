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

#include "screens/screens.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "mdi_codepoints.hpp"
#include "screens/common.hpp"

namespace btclock {

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

// Ref chars for the "ZAP" label panel. Matches the uppercase + digit
// span every other text-label screen renders against, so the ZAP label
// row baseline visually aligns with MSCW / BTC/SUPPLY / BITAXE.
constexpr const char* kLabelRef =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Ref chars for the amount digits — includes the suffix letters so a
// "21k" / "1.2M" tail shares a baseline with a plain "100".
constexpr const char* kAmountRef = "0123456789kMB.";

// ZAP@0, bolt@1, [blanks 2..N-3], sats glyph just before the amount,
// amount right-justified ending at N-1. Same anchor positions on 7-
// and 8-panel boards — V8's extra cell becomes another blank between
// the bolt and the glyph, keeping the left-edge ZAP/bolt pair stable
// across variants. Zero-amount-cells output means the layout degraded
// to "no room for digits" — caller paints the amount at the rightmost
// panel and skips the glyph.
struct ZapLayout {
  std::size_t zap_slot;       // ZAP label.
  std::size_t bolt_slot;      // MDI lightning-bolt icon.
  std::size_t glyph_slot;     // Sats glyph (one before first amount).
  std::size_t first_amount;   // Leftmost amount-digit slot.
  std::size_t amount_cells;   // Count of amount cells (= N - first_amount).
};

template <std::size_t N>
ZapLayout ComputeZapLayout(std::size_t amount_chars, bool use_sats_symbol) {
  ZapLayout L{};
  L.zap_slot = 0;
  L.bolt_slot = L.zap_slot + 1;
  // Reserve one slot for the sats glyph (when on); the amount fills the
  // tail starting at first_amount and ending at N-1. Clamp amount_chars
  // so first_amount can't slide past the bolt slot.
  const std::size_t glyph_reserve = use_sats_symbol ? 1 : 0;
  const std::size_t available_tail = N - (L.bolt_slot + 1);
  std::size_t amount = amount_chars;
  if (amount + glyph_reserve > available_tail) {
    amount = available_tail > glyph_reserve ? available_tail - glyph_reserve
                                            : available_tail;
  }
  if (amount < 1) amount = 1;
  L.first_amount = N - amount;
  L.amount_cells = amount;
  L.glyph_slot = (use_sats_symbol && L.first_amount > L.bolt_slot + 1)
                     ? L.first_amount - 1
                     : N;  // sentinel "no glyph slot"
  return L;
}

}  // namespace

template <size_t N>
void RenderNostrZapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296],
    const AppFonts& fonts,
    const DataSnapshot::LatestZap& zap,
    bool use_sats_symbol,
    uint8_t sats_variant,
    bool full_refresh_mode,
    bool vertical_desc) {
  static_assert(N >= 7, "Zap layout needs at least 7 panels");

  const std::string amount = FormatZapAmount(zap.amount_sats);
  const ZapLayout L = ComputeZapLayout<N>(amount.size(), use_sats_symbol);

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
    const auto glyph = SatsGlyphUtf8(sats_variant);
    PaintSlot g{};
    g.kind = PaintSlot::kSatsGlyph;
    g.text = glyph.c_str();
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

  // Always full-refresh. The middle panels (between the bolt slot and
  // the sats-glyph slot) are kBlank, and PaintDataScreen treats
  // kBlank+update=true as a partial-refresh no-op (skip clear, skip
  // refresh). On screen entry that means the previous screen's digits
  // bleed through the empty middle cells — symptom: "two amounts, one
  // before the sats glyph and one after". Full refresh wipes those
  // cells to white as part of the kBlank-on-full-refresh path.
  (void)full_refresh_mode;
  PaintDataScreen(panels, fb_storage, fonts, slots, update,
                  /*full_refresh=*/true, vertical_desc);
}

template void RenderNostrZapScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&, bool, uint8_t, bool,
    bool);
template void RenderNostrZapScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::LatestZap&, bool, uint8_t, bool,
    bool);

}  // namespace btclock
