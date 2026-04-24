#include "screens/screens.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "screens/common.hpp"

namespace btclock {

// parseMarketCap port. Two modes:
//
//   big_chars=true  — "<sym><N.NN>T" formatted via FormatNumberWithSuffix,
//                     right-padded with blanks, one character per panel.
//                     Matches old firmware's bigChars branch and the
//                     parity helper RenderMarketCapBigChars.
//
//   big_chars=false — 3-digit-group small-chars layout. Each trailing
//                     panel renders 3 digits at medium font so a 13-digit
//                     USD cap (1.5T) fits across 5 panels; a currency
//                     separator " $ " / " € " sits one slot ahead of the
//                     first group. Mirror is SmallCharsGroups in
//                     screen_math; /api/status data[] paints the same
//                     cells so the WebUI and EPD agree.
//
// Currency glyph rendering: the old firmware encodes CCY as a single
// byte ('$', '[', ']', '^'); we map to the UTF-8 form via
// CurrencySymbolUtf8 so the digit panel shows the correct glyph on
// device. WebUI's /api/status data[] still reflects the one-char form
// (via panel_texts.cpp) — see test_panel_texts.cpp for parity.

namespace {

// Decide which character a bigChars panel should display at position i
// within the digit-panel row. The formatted "1.02T" ladder is:
//   digits_slot = [padding..][<sym>][<num...>][<suffix>]
// We return the raw char at that slot (space for blanks). Callers that
// need the currency-glyph panel rendered as a UTF-8 symbol check
// is_currency_glyph separately.
struct MarketCapBigCell {
  char c;
  bool is_currency_glyph;
};

// Build the digit-panel rendering for bigChars mode. The old firmware's
// layout is: prefix the suffix-form with the single-byte currency, pad
// with leading spaces to exactly NUM_SCREENS (7) chars, then copy chars
// [1..6] into slots [1..6]. Here kDigitPanels == N - 1 == 6 on a 7-board
// and 7 on the 8-board — we pad to (1 + kDigitPanels) and skip the first
// slot (reserved for the MCAP label in slot 0).
template <size_t kDigitPanels>
void LayoutMarketCapBigChars(uint64_t cap, char currency_byte,
                             MarketCapBigCell (&cells)[kDigitPanels]) {
  // num_chars budget leaves room for the '$' prefix and one trailing
  // blank — matches parseMarketCap's (NUM_SCREENS - 2) argument.
  const int num_chars =
      static_cast<int>((kDigitPanels + 1) - 2);
  std::string formatted = FormatNumberWithSuffix(cap, num_chars);
  std::string s = std::string(1, currency_byte) + formatted;
  const size_t full_slots = kDigitPanels + 1;
  if (s.size() < full_slots) {
    s.insert(s.begin(), full_slots - s.size(), ' ');
  } else if (s.size() > full_slots) {
    // Overflow (shouldn't happen at reasonable caps, but guard):
    // keep the tail so the magnitude digits stay visible.
    s = s.substr(s.size() - full_slots);
  }
  // Find which slot holds the currency glyph so we can render it as a
  // UTF-8 symbol (not the raw byte).
  size_t glyph_slot = std::string::npos;
  for (size_t i = 0; i < s.size(); ++i) {
    if (static_cast<unsigned char>(s[i]) == static_cast<unsigned char>(currency_byte)) {
      glyph_slot = i;
      break;
    }
  }
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t src = i + 1;  // slot 0 is reserved for MCAP label
    cells[i].c = s[src];
    cells[i].is_currency_glyph = (src == glyph_slot);
  }
}

}  // namespace

template <size_t N>
void RenderMarketCapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    uint32_t block_height, const std::string& prev_price,
    uint32_t prev_height, bool big_chars) {
  static_assert(N >= 7, "market-cap layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = prev_price.empty() || prev_height == 0;

  const int32_t new_price = PriceInt(price);
  const int32_t old_price = full_refresh ? -1 : PriceInt(prev_price);
  const uint64_t now_cap =
      new_price < 0
          ? 0
          : MarketCap(static_cast<uint32_t>(new_price), block_height);
  const uint64_t prev_cap =
      full_refresh || old_price < 0
          ? 0
          : MarketCap(static_cast<uint32_t>(old_price), prev_height);

  // The old firmware wraps the suffix string with a single-byte currency
  // symbol — on device we paint a UTF-8 glyph instead, so pick the byte
  // here for layout math and swap to UTF-8 when rendering.
  const char currency_byte = [&]() -> char {
    if (currency == "USD") return '$';
    if (currency == "EUR") return '[';
    if (currency == "GBP") return ']';
    if (currency == "JPY") return '^';
    return '$';
  }();
  const char* currency_utf8 = CurrencySymbolUtf8(currency);

  MarketCapBigCell new_cells[kDigitPanels];
  MarketCapBigCell old_cells[kDigitPanels];
  std::vector<std::string> new_sc_cells;
  std::vector<std::string> old_sc_cells;
  // Separator cell literal: matches SmallCharsGroups' " X " form so
  // the mirror and renderer diff the same bytes. Rendered as a UTF-8
  // glyph below when currency_utf8 is available.
  const std::string ccy_cell = std::string(" ") + currency_byte + " ";

  if (big_chars) {
    LayoutMarketCapBigChars<kDigitPanels>(now_cap, currency_byte, new_cells);
    if (!full_refresh) {
      LayoutMarketCapBigChars<kDigitPanels>(prev_cap, currency_byte, old_cells);
    }
  } else {
    new_sc_cells = SmallCharsGroups(now_cap, ccy_cell, kDigitPanels);
    if (!full_refresh) {
      old_sc_cells = SmallCharsGroups(prev_cap, ccy_cell, kDigitPanels);
    }
  }

  std::array<PaintSlot, N> slots{};
  std::array<bool, N> update{};

  // Panel 0 — "<CCY>/MCAP" label. Static within a currency across price
  // updates, so only paint on full refresh.
  slots[0] = PaintSlot{PaintSlot::kLabelSplit,
                       currency + std::string("/MCAP"), nullptr, 0, 0};
  update[0] = full_refresh;

  // Digit / currency-glyph / 3-digit-group cells. big_chars branch maps
  // to kDigit / kCurrencyGlyph at 180 pt; small-chars branch maps to
  // kSmallGroup at 90 pt (and kCurrencyGlyph painted at the small_chars
  // 90 pt via a local kCurrencyGlyph analogue — see below).
  for (size_t i = 0; i < kDigitPanels; ++i) {
    const size_t panel_idx = 1 + i;
    if (big_chars) {
      const auto& cell = new_cells[i];
      if (cell.c == ' ') {
        slots[panel_idx] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
      } else if (cell.is_currency_glyph && currency_utf8[0] != '\0') {
        slots[panel_idx] = PaintSlot{PaintSlot::kCurrencyGlyph,
                                     std::string(currency_utf8),
                                     nullptr, 0, 0};
      } else {
        slots[panel_idx] = PaintSlot{PaintSlot::kDigit,
                                     std::string(1, cell.c),
                                     nullptr, 0, 0};
      }
      update[panel_idx] =
          full_refresh || cell.c != old_cells[i].c ||
          cell.is_currency_glyph != old_cells[i].is_currency_glyph;
    } else {
      const auto& cell = new_sc_cells[i];
      if (cell.empty()) {
        slots[panel_idx] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
      } else if (cell == ccy_cell && currency_utf8[0] != '\0') {
        // Currency separator rendered at small_chars 90 pt so weight
        // matches the 3-digit groups either side. Piggy-back on
        // kSmallGroup (same font + size).
        slots[panel_idx] = PaintSlot{PaintSlot::kSmallGroup,
                                     std::string(currency_utf8),
                                     nullptr, 0, 0};
      } else if (cell == " ") {
        slots[panel_idx] = PaintSlot{PaintSlot::kBlank, "", nullptr, 0, 0};
      } else {
        slots[panel_idx] =
            PaintSlot{PaintSlot::kSmallGroup, cell, nullptr, 0, 0};
      }
      update[panel_idx] = full_refresh || cell != old_sc_cells[i];
    }
  }

  PaintDataScreen(panels, fb_storage, fonts, slots, update, full_refresh);
}

template void RenderMarketCapScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    uint32_t, const std::string&, uint32_t, bool);
template void RenderMarketCapScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    uint32_t, const std::string&, uint32_t, bool);

}  // namespace btclock
