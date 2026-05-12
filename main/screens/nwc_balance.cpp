// NWC wallet balance screen. Rotatable, partial-refresh capable.
// Layout (7- and 8-panel, mirrors NostrZap so the two NWC screens
// share a visual identity):
//   [BAL] [bolt] [ ] ... [ ] [sats glyph] [amount digits...]
//
// BAL + bolt anchor at the leftmost two cells on every variant. The
// MDI lightning-bolt cell is the brand cue across NWC screens
// (balance + payment notify). The amount is right-justified into the
// remaining tail with the sats glyph one slot before the most-
// significant digit; long balances spill leftward through the blanks
// and drop the glyph when the digit run would otherwise overflow.

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "mdi_codepoints.hpp"
#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {

std::string FormatSatsCompact(const std::optional<int64_t>& amount_sats,
                              std::size_t max_int_cells) {
  if (!amount_sats || *amount_sats < 0) return "—";
  const int64_t v = *amount_sats;
  // BTC rendering kicks in past 100M sats — keeps a wallet >= 1 BTC
  // readable as "1.0000" rather than collapsing to "1B" via the
  // FormatZapAmount suffix path. 4 decimals matches the Satoshi-
  // precision threshold the WebUI uses for its own balance display.
  // Fall back to FormatZapAmount only for the sub-100M-sat band so
  // the suffix path covers small balances (1234 → "1234", 1.5M → "1.5M").
  if (v >= 100'000'000LL) {
    const double btc = static_cast<double>(v) / 1e8;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.4f", btc);
    std::string s = buf;
    if (s.size() <= max_int_cells) return s;
    // Wallet > ~10k BTC — defensive fallback to a coarse "Nk₿" form;
    // the suffix path's k/M/B logic on BTC values keeps the display
    // legible without inventing a separate formatter.
    return FormatZapAmount(amount_sats, max_int_cells);
  }
  return FormatZapAmount(amount_sats, max_int_cells);
}

namespace {

// Ref chars matching the NostrZap label cell so the BAL label
// baseline aligns visually with the ZAP overlay on switch-over.
constexpr const char* kLabelRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
// Ref chars for the amount digits — includes the suffix letters and
// the BTC '.' so a "1.0000" and a "1.2M" share a baseline.
constexpr const char* kAmountRef = "0123456789kMB.";

// Shared layout for the balance and payment-notify screens. Mirror of
// nostr_zap.cpp's ComputeZapLayout — the two screens share a visual
// language so the layout helper does too. Kept anonymous-namespace
// local here because nostr_zap's helper is also anonymous; sharing
// the type would either require a header lift-out or a duplicated
// signature, and the body is small enough that duplication beats
// the include surface change.
struct NwcLayout {
  std::size_t label_slot;    // BAL / GOT / PAID
  std::size_t bolt_slot;     // MDI lightning-bolt icon
  std::size_t glyph_slot;    // Sats glyph (one before first amount)
  std::size_t first_amount;  // Leftmost amount-digit slot
  std::size_t amount_cells;  // Count of amount cells
};

template <std::size_t N>
NwcLayout ComputeNwcLayout(std::size_t amount_chars, bool reserve_glyph_cell) {
  NwcLayout L{};
  L.label_slot = 0;
  L.bolt_slot = L.label_slot + 1;
  const std::size_t available_tail = N - (L.bolt_slot + 1);
  std::size_t amount = amount_chars;
  if (amount > available_tail) amount = available_tail;
  if (amount < 1) amount = 1;
  L.first_amount = N - amount;
  L.amount_cells = amount;
  L.glyph_slot = (reserve_glyph_cell && L.first_amount > L.bolt_slot + 1)
                     ? L.first_amount - 1
                     : N;
  return L;
}

template <std::size_t N>
void RenderNwcShared(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
                     uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
                     const char* label, std::uint32_t icon_codepoint,
                     const std::string& amount, bool use_sats_symbol,
                     bool use_btc_symbol, uint8_t sats_variant,
                     bool full_refresh_mode, bool vertical_desc,
                     const std::array<bool, N>& update) {
  const bool reserve_glyph = use_sats_symbol || use_btc_symbol;
  const NwcLayout L = ComputeNwcLayout<N>(amount.size(), reserve_glyph);

  std::array<PaintSlot, N> slots{};
  for (std::size_t i = 0; i < N; ++i) slots[i].kind = PaintSlot::kBlank;

  PaintSlot lbl{};
  lbl.kind = PaintSlot::kLabel;
  lbl.text = label;
  lbl.ref_override = kLabelRef;
  slots[L.label_slot] = lbl;

  // Caller-supplied icon — bolt for balance/zap, direction-aware arrow
  // (up/down) for payment-notify. Zero codepoint leaves the cell blank
  // (defensive — current call sites always provide a valid glyph).
  if (icon_codepoint != 0) {
    PaintSlot icon{};
    icon.kind = PaintSlot::kMdiIcon;
    icon.mdi_codepoint = icon_codepoint;
    slots[L.bolt_slot] = icon;
  }

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

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh_mode,
                  vertical_desc);
}

}  // namespace

template <size_t N>
void RenderNwcBalanceScreen(
    std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::optional<int64_t>& balance_sats,
    const std::optional<int64_t>& prev_balance_sats, bool use_sats_symbol,
    bool use_btc_symbol, uint8_t sats_variant, bool full_refresh_mode,
    bool vertical_desc) {
  static_assert(N >= 7, "NWC balance layout needs at least 7 panels");

  // Tail budget excludes BAL + bolt. The glyph isn't reserved here so
  // the integer rendering can fill the tail when it fits — the layout
  // step drops the glyph instead of truncating digits.
  constexpr std::size_t kAmountBudget = N - 2;
  const std::string amount = FormatSatsCompact(balance_sats, kAmountBudget);
  const std::string prev_amount =
      prev_balance_sats ? FormatSatsCompact(prev_balance_sats, kAmountBudget)
                        : std::string();

  // Diff drives the per-cell update flags so a refreshed poll only
  // repaints the cells that actually changed. Full refresh path (first
  // boot, force-full) sets every cell true upstream.
  std::array<bool, N> update{};
  const bool full = full_refresh_mode || prev_amount.empty();
  for (std::size_t i = 0; i < N; ++i) update[i] = full;
  if (!full) {
    // Recompute layouts to drive a per-slot diff. The amount string may
    // shift left/right between updates (digit-count change), so a
    // simple position-wise compare on the raw strings would over-report
    // changes. Drive the diff off the post-pad amount cell content.
    const bool reserve_glyph = use_sats_symbol || use_btc_symbol;
    const NwcLayout L_now = ComputeNwcLayout<N>(amount.size(), reserve_glyph);
    const NwcLayout L_prev =
        ComputeNwcLayout<N>(prev_amount.size(), reserve_glyph);
    if (L_now.first_amount != L_prev.first_amount ||
        L_now.glyph_slot != L_prev.glyph_slot) {
      // Layout shifted — repaint every tail cell. Cheaper than trying
      // to compute the union of changes.
      for (std::size_t i = L_now.bolt_slot + 1; i < N; ++i) update[i] = true;
    } else {
      const std::size_t pad_now = (L_now.amount_cells > amount.size())
                                      ? L_now.amount_cells - amount.size()
                                      : 0;
      const std::size_t pad_prev = (L_prev.amount_cells > prev_amount.size())
                                       ? L_prev.amount_cells - prev_amount.size()
                                       : 0;
      for (std::size_t i = 0; i < L_now.amount_cells; ++i) {
        const std::size_t panel_idx = L_now.first_amount + i;
        const char now_ch = (i < pad_now) ? ' ' : amount[i - pad_now];
        const char prev_ch =
            (i < pad_prev) ? ' ' : prev_amount[i - pad_prev];
        if (now_ch != prev_ch) update[panel_idx] = true;
      }
    }
  }

  RenderNwcShared<N>(panels, fb_storage, fonts, "BAL",
                     mdi::kIconLightningBolt, amount, use_sats_symbol,
                     use_btc_symbol, sats_variant, full_refresh_mode,
                     vertical_desc, update);
}

template <size_t N>
void RenderNwcPaymentNotifyScreen(
    std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const DataSnapshot::NwcPayment& payment, bool use_sats_symbol,
    bool use_btc_symbol, uint8_t sats_variant, bool full_refresh_mode,
    bool vertical_desc) {
  static_assert(N >= 7, "NWC payment notify layout needs at least 7 panels");

  constexpr std::size_t kAmountBudget = N - 2;
  const std::string amount = FormatSatsCompact(payment.amount_sats, kAmountBudget);

  // direction == 2 → outgoing → "PAID" + arrow-up; ==1 → incoming
  // "GOT" + arrow-down; anything else → "GOT" with the bolt fallback
  // so an unknown direction still paints a sensible glyph rather than
  // leaving the icon slot blank.
  const char* label = (payment.direction == 2) ? "PAID" : "GOT";
  std::uint32_t icon = mdi::kIconLightningBolt;
  if (payment.direction == 1) {
    icon = mdi::kIconArrowDownBold;
  } else if (payment.direction == 2) {
    icon = mdi::kIconArrowUpBold;
  }

  std::array<bool, N> update{};
  for (std::size_t i = 0; i < N; ++i) update[i] = true;

  RenderNwcShared<N>(panels, fb_storage, fonts, label, icon, amount,
                     use_sats_symbol, use_btc_symbol, sats_variant,
                     full_refresh_mode, vertical_desc, update);
}

template void RenderNwcBalanceScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::optional<int64_t>&,
    const std::optional<int64_t>&, bool, bool, uint8_t, bool, bool);
template void RenderNwcBalanceScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::optional<int64_t>&,
    const std::optional<int64_t>&, bool, bool, uint8_t, bool, bool);

template void RenderNwcPaymentNotifyScreen<7>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot::NwcPayment&, bool, bool, uint8_t,
    bool, bool);
template void RenderNwcPaymentNotifyScreen<8>(
    std::array<std::unique_ptr<epd::IEpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot::NwcPayment&, bool, bool, uint8_t,
    bool, bool);

}  // namespace btclock
