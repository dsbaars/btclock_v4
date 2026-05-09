#include "screens/panel_texts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "screens/assets/pool_logos.hpp"
#include "screens/btc_price_suffix_layout.hpp"
#include "screens/fee_rate_layout.hpp"
#include "screens/price_layout.hpp"
#include "screens/sats_per_currency_layout.hpp"
#include "screens/screen_math.hpp"

namespace btclock {

namespace {

constexpr const char kPanelTextsBtcSignUtf8[] = "\xe2\x82\xbf";

}  // namespace

// Compact "<value><unit>" string for a GH/s hashrate. A Bitaxe Gamma
// sits around 1.2 THz; older gens are sub-500 GH. 1 PH is the top of
// the scale we support — beyond that a single device is off-list and
// the display truncates to "999T" rather than wrapping to "1EH".
std::string FormatBitaxeHashrate(double ghs) {
  const BitaxeHashrateParts parts = SplitBitaxeHashrate(ghs);
  return parts.value + parts.suffix;
}

BitaxeHashrateParts SplitBitaxeHashrate(double ghs) {
  BitaxeHashrateParts out;
  if (!(ghs > 0.0)) {
    out.value = "0";
    out.suffix = "GH";
    return out;
  }
  char buf[24];
  // Pick the smallest unit that keeps the integer part under three
  // digits so the widget never overflows on the EPD. The suffix is
  // stripped from the formatted string so callers can render the unit
  // as its own panel (split-text "<suffix>/S") while the digits stay
  // in the value slots.
  if (ghs < 1000.0) {
    std::snprintf(buf, sizeof(buf), "%.0f", std::round(ghs));
    out.suffix = "GH";
  } else if (ghs < 1'000'000.0) {
    const double t = ghs / 1000.0;
    // Below 10 TH keep one decimal so 1.2/1.5/2.1 TH stays readable.
    if (t < 10.0) {
      std::snprintf(buf, sizeof(buf), "%.1f", t);
    } else {
      std::snprintf(buf, sizeof(buf), "%.0f", std::round(t));
    }
    out.suffix = "TH";
  } else {
    const double p = ghs / 1'000'000.0;
    if (p < 10.0) {
      std::snprintf(buf, sizeof(buf), "%.1f", p);
    } else {
      std::snprintf(buf, sizeof(buf), "%.0f", std::round(p));
    }
    out.suffix = "PH";
  }
  out.value = buf;
  return out;
}

namespace {

// Local copies of the pure parse helpers declared in common.hpp. Using
// them here would force common.hpp on callers (e.g. screen_manager.cpp),
// which drags the EPD headers in — duplicating the tiny arithmetic keeps
// panel_texts header-pure. Behaviour must track common.cpp; the host
// test `test_panel_texts.cpp` pins the shared surface.
int32_t SatsPerUnitLocal(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p <= 0.0 || endp == price_str.c_str()) return -1;
  const double sats = 1e8 / p;
  if (sats > 4e9) return -1;
  return static_cast<int32_t>(sats + 0.5);
}

int32_t PriceIntLocal(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p < 0.0 || endp == price_str.c_str()) return -1;
  if (p > 2e9) return -1;
  return static_cast<int32_t>(p + 0.5);
}

// Same parse as PriceIntLocal but preserves the fractional part — the
// price-layout helper does its own decimal-count decision per magnitude.
double PriceDoubleLocal(const std::string& price_str) {
  if (price_str.empty()) return -1.0;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (endp == price_str.c_str()) return -1.0;
  if (!(p >= 0.0)) return -1.0;
  if (p > 2e9) return -1.0;
  return p;
}

// UTF-8 currency symbol for the given ISO code. Mirrors common.cpp's
// CurrencySymbolUtf8 (kept local for header purity). For codes outside
// the dedicated-glyph set (USD/EUR/GBP/JPY/CAD/AUD), returns the ISO
// code itself — a runtime-fetched catalogue from /api/v2/currencies can
// include codes the firmware has no single-char glyph for (CHF, BRL,
// INR, …), and rendering the code keeps the currency-glyph cell non-
// empty so the panel-text mirror agrees byte-for-byte with what the
// on-device renderer (CurrencySymbolUtf8) paints.
std::string CurrencySymbolLocal(const std::string& ccy) {
  if (ccy == "USD") return "$";
  if (ccy == "EUR") return "\xE2\x82\xAC";
  if (ccy == "GBP") return "\xC2\xA3";
  if (ccy == "JPY") return "\xC2\xA5";
  if (ccy == "CAD") return "$";
  if (ccy == "AUD") return "$";
  if (ccy.empty()) return "";
  return ccy;
}

// Single-char slot for a digit; empty string for ' ' padding. Keeps the
// WebUI rendering "  2 6 8 4" as the same blank pattern the old firmware
// produced — an empty slot visually matches a `' '` character.
std::string CharSlot(char c) {
  if (c == ' ') return std::string();
  return std::string(1, c);
}

// Fill N digit strings from a right-justified char array. `digits` must
// have at least `slots` entries; `out_texts` gets `slots` entries
// appended. Reuses CharSlot so ' ' → "".
void AppendDigits(const char* digits, std::size_t slots,
                  std::vector<std::string>& out) {
  for (std::size_t i = 0; i < slots; ++i) out.push_back(CharSlot(digits[i]));
}

// --- Per-kind builders ---

std::vector<std::string> BuildBlockHeight(uint32_t h, std::size_t n_panels) {
  // Label occupies slot 0 unless the integer needs every slot (e.g. a
  // 7-digit height on a 7-panel board). The old firmware's
  // parseBlockHeight dropped the label in that overflow case, so we do
  // the same — the WebUI only sees digit cells, no label.
  std::vector<std::string> out;
  out.reserve(n_panels);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
  const std::size_t len = std::strlen(buf);
  if (len >= n_panels) {
    // Overflow: one digit per slot, truncate leading digits if >n.
    const std::size_t start = (len > n_panels) ? len - n_panels : 0;
    for (std::size_t i = 0; i < n_panels; ++i) {
      out.emplace_back(1, buf[start + i]);
    }
    return out;
  }
  out.emplace_back("BLOCK/HEIGHT");
  const std::size_t digit_slots = n_panels - 1;
  std::vector<char> digits(digit_slots);
  FormatDigits64(static_cast<uint64_t>(h), digits.data(), digit_slots);
  AppendDigits(digits.data(), digit_slots, out);
  return out;
}

std::vector<std::string> BuildHalving(uint32_t h, std::size_t n_panels,
                                      bool as_blocks) {
  std::vector<std::string> out;
  out.reserve(n_panels);
  if (as_blocks) {
    const uint32_t rem = HalvingCountdown(h);
    out.emplace_back("HAL/VING");
    const std::size_t digit_slots = n_panels - 1;
    std::vector<char> digits(digit_slots);
    FormatDigits64(static_cast<uint64_t>(rem), digits.data(), digit_slots);
    AppendDigits(digits.data(), digit_slots, out);
    return out;
  }
  // Time-mode: "BIT/COIN | HAL/VING | N/YRS | N/DAYS | N/HRS | N/MINS | TO/GO"
  // anchored at the trailing 7 slots. Boards wider than 7 get leading blank
  // slots. Matches halving.cpp's 7-slot layout exactly.
  const HalvingTimeBreakdown tb = HalvingCountdownBreakdown(h);
  std::array<std::string, 7> s;
  s[0] = "BIT/COIN";
  s[1] = "HAL/VING";
  s[7 - 5] = std::to_string(tb.years) + "/YRS";
  s[7 - 4] = std::to_string(tb.days) + "/DAYS";
  s[7 - 3] = std::to_string(tb.hours) + "/HRS";
  s[7 - 2] = std::to_string(tb.minutes) + "/MINS";
  s[7 - 1] = "TO/GO";
  if (n_panels < 7) {
    // Defensive: truncate the head (never happens on current hardware).
    for (std::size_t i = 7 - n_panels; i < 7; ++i) out.push_back(s[i]);
    return out;
  }
  for (std::size_t i = 0; i < n_panels - 7; ++i) out.emplace_back();
  for (std::size_t i = 0; i < 7; ++i) out.push_back(s[i]);
  return out;
}

// UTF-8 aware split: one vector entry per codepoint. Used by big-chars
// emit so multi-byte glyphs (e.g. the €, £, ¥ currency signs) land on a
// single panel instead of being spread one-byte-per-cell — the on-device
// renderer draws them as one glyph but the /api/status `data` mirror
// must agree cell-for-cell, otherwise the web client sees surrogate
// escapes where the currency symbol should be.
std::vector<std::string> SplitUtf8Codepoints(const std::string& s) {
  std::vector<std::string> out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((lead & 0xE0) == 0xC0)
      len = 2;
    else if ((lead & 0xF0) == 0xE0)
      len = 3;
    else if ((lead & 0xF8) == 0xF0)
      len = 4;
    if (i + len > s.size()) len = s.size() - i;
    out.emplace_back(s.substr(i, len));
    i += len;
  }
  return out;
}

// Big-chars tail layout: slot 0 is the caller-supplied label, slots
// 1..n_panels-1 carry `s` one codepoint per cell, right-aligned so
// short values get blank cells on the left. Matches the old firmware's
// visual layout and — critically — keeps multi-byte glyphs whole when
// the result is serialised into /api/status `data`.
std::vector<std::string> EmitBigCharsFrame(std::string label, std::string s,
                                           std::size_t n_panels) {
  std::vector<std::string> out;
  out.reserve(n_panels);
  out.emplace_back(std::move(label));
  if (n_panels <= 1) return out;
  const std::size_t tail_slots = n_panels - 1;
  std::vector<std::string> cells = SplitUtf8Codepoints(s);
  if (cells.size() < tail_slots) {
    cells.insert(cells.begin(), tail_slots - cells.size(), std::string(" "));
  } else if (cells.size() > tail_slots) {
    cells.erase(cells.begin(), cells.begin() + (cells.size() - tail_slots));
  }
  for (const auto& c : cells) {
    // CharSlot's ' ' → "" contract keeps blank cells visually equal
    // across ASCII-only and multi-byte inputs.
    out.push_back(c == " " ? std::string() : c);
  }
  return out;
}

// Three-digit-group "small chars" layout shared by supply and market-cap
// non-bigChars modes. Spaces out the number into groups of 3 digits
// across the tail slots; earlier slots are blank; slot[0] carries the
// caller-supplied label. When a non-empty `ccy_cell` is given it's
// placed in the slot just ahead of the first digit group (used by
// market-cap to render the " <CCY> " separator).
std::vector<std::string> EmitSmallCharsGroups(std::string label, uint64_t value,
                                              const std::string& ccy_cell,
                                              std::size_t n_panels) {
  // Shared implementation lives in screen_math so the device renderers
  // emit byte-identical cells. Label occupies the first slot; the rest
  // is filled by SmallCharsGroups.
  std::vector<std::string> out(n_panels);
  out[0] = std::move(label);
  if (n_panels <= 1) return out;
  auto groups = SmallCharsGroups(value, ccy_cell, n_panels - 1);
  for (std::size_t i = 0; i < groups.size(); ++i) {
    out[1 + i] = std::move(groups[i]);
  }
  return out;
}

std::vector<std::string> BuildBitcoinSupply(uint32_t h, bool big_chars,
                                            bool show_percent,
                                            std::size_t n_panels) {
  const uint64_t supply = SupplyAtBlock(h);
  if (show_percent) {
    // "NN.NN" spread over the inner panels + "%" trailer. The renderer
    // (RenderBitcoinSupplyScreen) paints that as a full-size '%' digit.
    std::vector<std::string> out;
    out.reserve(n_panels);
    out.emplace_back("BTC/SUPPLY");
    const double frac =
        std::round((static_cast<double>(supply) / 20999999.9769) * 10000.0) /
        100.0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f%%", frac);
    std::string s = buf;
    if (s.size() < n_panels) s.insert(s.begin(), n_panels - s.size(), ' ');
    for (std::size_t i = 1; i < n_panels; ++i) {
      out.push_back(CharSlot(s[i]));
    }
    if (!out.empty()) out.back() = "%";
    return out;
  }
  if (big_chars) {
    // Budget = n_panels-1 so the magnitude fills every tail cell — at
    // 7 panels that gives "20.02M" instead of "20.0M" with a leading
    // blank cell. The label stays in slot 0.
    std::string s =
        FormatNumberWithSuffix(supply, static_cast<int>(n_panels) - 1);
    return EmitBigCharsFrame("BTC/SUPPLY", std::move(s), n_panels);
  }
  // Small-chars three-digit groups. Parity: RenderBitcoinSupplySmallChars.
  return EmitSmallCharsGroups("BTC/SUPPLY", supply, "", n_panels);
}

std::vector<std::string> BuildMarketCap(uint32_t h, const std::string& price,
                                        const std::string& currency,
                                        bool big_chars, bool share_dot,
                                        std::size_t n_panels) {
  const int32_t pi = PriceIntLocal(price);
  const uint64_t cap = (pi < 0) ? 0 : MarketCap(static_cast<uint32_t>(pi), h);
  if (big_chars) {
    // Label + "<sym><N.NN>T" big-chars suffix form. share_dot bumps the
    // formatter budget by one and folds "." into its preceding cell so
    // a magnitude like "$1.02T" becomes ["$","1.","0","2","T"] instead
    // of ["$","1",".","0","T"] — same trick as the BTC-price suffix
    // layout, applied to the EmitBigCharsFrame tail.
    const std::string glyph = CurrencySymbolLocal(currency);
    const int budget = static_cast<int>(n_panels) - (share_dot ? 1 : 2);
    std::string s = glyph + FormatNumberWithSuffix(cap, budget);
    if (!share_dot) {
      return EmitBigCharsFrame(currency + "/MCAP", std::move(s), n_panels);
    }
    // Build the cell vector with the fold applied, then mirror
    // EmitBigCharsFrame's tail layout (right-align, blanks become "").
    std::vector<std::string> cells = SplitUtf8Codepoints(s);
    const std::size_t dot_pos = [&]() {
      for (std::size_t i = 0; i < cells.size(); ++i) {
        if (cells[i] == ".") return i;
      }
      return std::string::npos;
    }();
    if (dot_pos != std::string::npos && dot_pos > 0) {
      cells[dot_pos - 1] = cells[dot_pos - 1] + ".";
      cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(dot_pos));
    }
    std::vector<std::string> out;
    out.reserve(n_panels);
    out.emplace_back(currency + "/MCAP");
    if (n_panels <= 1) return out;
    const std::size_t tail_slots = n_panels - 1;
    if (cells.size() < tail_slots) {
      cells.insert(cells.begin(), tail_slots - cells.size(), std::string(" "));
    } else if (cells.size() > tail_slots) {
      cells.erase(cells.begin(),
                  cells.begin() +
                      static_cast<std::ptrdiff_t>(cells.size() - tail_slots));
    }
    for (const auto& c : cells) {
      out.push_back(c == " " ? std::string() : c);
    }
    return out;
  }
  // Small-chars: three-digit groups across the trailing slots, with a
  // " <CCY> " separator cell just before the first group. Matches
  // RenderMarketCapSmallChars (test_datahandler_parity.cpp).
  const std::string glyph = CurrencySymbolLocal(currency);
  std::string ccy_cell = std::string(" ") + glyph + " ";
  return EmitSmallCharsGroups(currency + "/MCAP", cap, ccy_cell, n_panels);
}

std::vector<std::string> BuildMoscowTime(const std::string& currency,
                                         const std::string& price,
                                         std::size_t n_panels,
                                         bool use_sats_symbol,
                                         bool use_btc_symbol,
                                         bool use_mscw_time, bool share_dot) {
  // Label rule mirrors RenderMoscowTimeScreen: USD with sats in the
  // classic Moscow-time range (0 < sats < 100_000) gets "MSCW/TIME";
  // everything else (other currencies, out-of-range sats, or
  // use_mscw_time=false) gets "SATS/<CCY>". The sats-glyph panel
  // (one slot before the first digit) is emitted as "STS" — the
  // same marker token parseSatsPerCurrency uses — unless
  // use_sats_symbol=false, in which case the marker slot stays blank.
  //
  // Digit-slot count is n_panels-1 — the renderer uses all N-1 slots
  // after the label (Bug 3 — the previous hard-coded 6-slot layout
  // left the trailing V8 panel blank).
  std::vector<std::string> out;
  out.reserve(n_panels);
  // Layout helper handles both the >= 1 sat-per-currency integer path
  // and the < 1 fractional "0.dddd" path. Moscow-time label only
  // applies to the integer path (USD, < 100k sats per dollar). The
  // fractional path keeps the SATS/<CCY> label so users with weak-fiat
  // currencies (VND/IRR/LBP) still see the currency identified.
  // share_dot is forwarded so the fractional path emits the merged
  // "0." cell when the user has decimalShareDot=true.
  const int32_t sats_int = SatsPerUnitLocal(price);
  const bool reserve_marker = use_sats_symbol || use_btc_symbol;
  const std::size_t digit_slots = (n_panels >= 1) ? n_panels - 1 : 0;
  // Run the layout in the runtime size by constructing the appropriate
  // template instantiation. n_panels is bounded to {7, 8} on every
  // shipping board; the kDigit slot count is digit_slots == 6 or 7.
  // Falling back to the 6-slot layout for any other size keeps the
  // mirror non-empty rather than crashing.
  bool fractional = false;
  std::vector<std::string> cells(digit_slots);
  std::vector<bool> is_sats(digit_slots, false);
  auto fill_from_layout = [&](auto layout) {
    fractional = layout.fractional;
    for (std::size_t i = 0; i < digit_slots; ++i) {
      cells[i] = layout.cells[i];
      is_sats[i] = layout.is_sats[i];
    }
  };
  if (digit_slots == 7) {
    fill_from_layout(
        ComputeSatsPerCurrencyLayout<7>(price, reserve_marker, share_dot));
  } else if (digit_slots == 6) {
    fill_from_layout(
        ComputeSatsPerCurrencyLayout<6>(price, reserve_marker, share_dot));
  } else if (digit_slots > 0) {
    // Defensive fallback — shouldn't be hit on shipping boards.
    fill_from_layout(
        ComputeSatsPerCurrencyLayout<6>(price, reserve_marker, share_dot));
  }

  const bool moscow = use_mscw_time && currency == "USD" && !fractional &&
                      sats_int > 0 && sats_int < 100000;
  out.emplace_back(moscow ? std::string("MSCW/TIME") : ("SATS/" + currency));

  for (std::size_t i = 0; i < digit_slots; ++i) {
    if (is_sats[i] && reserve_marker) {
      out.emplace_back(use_btc_symbol ? std::string(kPanelTextsBtcSignUtf8)
                                      : std::string("STS"));
    } else {
      // When use_sats_symbol=false the marker cell stays blank — a
      // stray "STS" on the suppressed side would disagree with what
      // the EPD paints. The fractional path emits cells already
      // populated with "0", "0.", ".", or single digit chars; the
      // integer path emits single digit chars or "" for blank pad.
      out.push_back(cells[i]);
    }
  }
  return out;
}

std::vector<std::string> BuildBtcPrice(const std::string& currency,
                                       const std::string& price,
                                       std::size_t n_panels, bool suffix_price,
                                       bool mow_mode, bool share_dot) {
  // Panel 0 = "BTC/<CCY>" (or "MOW/UNITS" on the suffix+mow path when
  // the price still fits with a label). Digits 1..N-1 come from the
  // layout helpers in price_layout.hpp / btc_price_suffix_layout.hpp —
  // same helpers the on-panel renderer uses, so the WebUI mirror and
  // the EPD agree on where the '.' lands, which cell carries the glyph,
  // and whether the glyph is dropped on overflow. Plain-path decimal
  // rules live in price_layout.hpp; suffix/MOW rules mirror v3
  // parsePriceData in lib/btclock/data_handler.cpp.
  std::vector<std::string> out;
  out.reserve(n_panels);
  // Materialise the local for stable .c_str() lifetime — the layout
  // helpers below take const char* and the returned cells reference
  // that pointer's bytes when the glyph is the ISO-code fallback.
  const std::string sym_storage = CurrencySymbolLocal(currency);
  const char* sym = sym_storage.c_str();
  const double pd = PriceDoubleLocal(price);

  // Suffix path fires when `suffix_price` is on OR the integer part of
  // the price is wide enough to force it — matching v3 parsePriceData's
  // `std::to_string(price).length() >= NUM_SCREENS || useSuffixFormat`
  // guard. We pre-check the width here so the mow_mode-without-suffix
  // short-price case falls through to the integer path (v3 behaviour).
  const int32_t price_int = PriceIntLocal(price);
  const bool integer_overflow =
      price_int >= 0 && std::to_string(price_int).size() >= n_panels;
  const bool go_suffix = (suffix_price || integer_overflow) && price_int >= 0;

  if (go_suffix) {
    std::string label;
    // On the overflow branch the suffix layout clears `label` and emits
    // the currency glyph into cells[0] — the on-panel renderer paints
    // that glyph on panel 0 in place of the dropped label (see
    // RenderBtcPriceScreen's kCurrencyGlyph slot-0 branch). The mirror
    // must agree cell-for-cell, otherwise /api/status carries "BTC/EUR"
    // in slot 0 while the EPD shows the € glyph. Fixes the Rev B
    // mowMode parity bug where `data[0]="BTC/EUR"` disagreed with the
    // physical "€" on panel 0.
    if (n_panels == 7) {
      constexpr std::size_t kPanels = 7;
      auto cells = LayoutBtcPriceSuffixStrings<kPanels>(
          static_cast<uint64_t>(price_int), currency, sym, mow_mode, share_dot,
          label);
      if (label.empty()) {
        // Overflow: glyph is already in cells[0].
        for (std::size_t i = 0; i < kPanels; ++i) out.push_back(cells[i]);
      } else {
        // Label path: paint label on panel 0, digits follow.
        out.emplace_back(label);
        for (std::size_t i = 1; i < kPanels; ++i) out.push_back(cells[i]);
      }
    } else if (n_panels == 8) {
      constexpr std::size_t kPanels = 8;
      auto cells = LayoutBtcPriceSuffixStrings<kPanels>(
          static_cast<uint64_t>(price_int), currency, sym, mow_mode, share_dot,
          label);
      if (label.empty()) {
        for (std::size_t i = 0; i < kPanels; ++i) out.push_back(cells[i]);
      } else {
        out.emplace_back(label);
        for (std::size_t i = 1; i < kPanels; ++i) out.push_back(cells[i]);
      }
    } else {
      out.emplace_back("BTC/" + currency);
      for (std::size_t i = 1; i < n_panels; ++i) out.emplace_back();
    }
    return out;
  }

  // Plain integer / sub-dollar-decimal path (v4 default). Also where we
  // land when `mow_mode=true && suffix_price=false` on a short price —
  // matches v3 parsePriceData which only consults `mowMode` inside the
  // suffix branch.
  out.emplace_back("BTC/" + currency);
  if (n_panels == 7) {
    constexpr std::size_t kSlots = 6;
    auto cells = LayoutBtcPriceStrings<kSlots>(pd, sym);
    for (const auto& s : cells) out.push_back(s);
  } else if (n_panels == 8) {
    constexpr std::size_t kSlots = 7;
    auto cells = LayoutBtcPriceStrings<kSlots>(pd, sym);
    for (const auto& s : cells) out.push_back(s);
  } else {
    for (std::size_t i = 1; i < n_panels; ++i) out.emplace_back();
  }
  return out;
}

std::vector<std::string> BuildFeeRate(const std::optional<double>& fee_opt,
                                      std::size_t n_panels) {
  // Panel 0 = "FEE/RATE" (split), panels 1..N-2 = digits, panel N-1 =
  // "sat/vB" (split). Both label slots use the same slash-delimited
  // encoding the WebUI already renders as a paired top/bottom stack
  // (matches BLOCK/HEIGHT, BTC/SUPPLY, etc). Bug 4 flipped the unit
  // from a cramped single-line "sat/vB" to "sat" over "vB" on device —
  // emit the same slash-encoded token here so the /api/status mirror
  // lines up with what the panel paints.
  std::vector<std::string> out;
  out.reserve(n_panels);
  out.emplace_back("FEE/RATE");
  const std::size_t digit_slots = (n_panels >= 2) ? n_panels - 2 : 0;
  const double fee = (fee_opt && *fee_opt >= 0.0) ? *fee_opt : -1.0;
  // LayoutFeeRate writes into a fixed-size array<char,Slots>. Go through
  // the 5-slot (7-panel) and 6-slot (8-panel) cases directly — the
  // firmware only ships these two topologies today.
  if (digit_slots == 5) {
    std::array<char, 5> digits{' ', ' ', ' ', ' ', ' '};
    LayoutFeeRate(fee, digits);
    AppendDigits(digits.data(), 5, out);
  } else if (digit_slots == 6) {
    std::array<char, 6> digits{' ', ' ', ' ', ' ', ' ', ' '};
    LayoutFeeRate(fee, digits);
    AppendDigits(digits.data(), 6, out);
  } else {
    for (std::size_t i = 0; i < digit_slots; ++i) out.emplace_back();
  }
  out.emplace_back("sat/vB");
  return out;
}

// Populate slot 0 with either the logo sentinel (empty — the renderer
// paints a bitmap there and has no textual representation; the WebUI is
// expected to render its own pool logo on the kind-aware path) or the
// pool name as a fallback. Single-word names render verbatim; names
// containing a whitespace/'_' delimiter emit "<left>/<right>" —
// matches the v3 getDisplayLabel() convention ("SATOSHI/RADIO",
// "PUBLIC/POOL") that the WebUI already renders as a split-text cell
// (same shape as "BLOCK/HEIGHT", "FEE/RATE", "sat/vB"). On the device
// this is drawn with DrawSplitText — horizontal line between the two
// halves.
std::string PoolLabelCellFor(const std::string& name) {
  // Lookup goes through the same case-insensitive ASCII fold the
  // renderer uses. When a vendored logo exists, the label cell is an
  // empty string — round-trips cleanly through the WebUI's `data[]`
  // renderer.
  // HasResolvedLogo asks the renderer's full chain (cache → vendored)
  // so the label cell stays blank as soon as a fetched bitmap lands —
  // matches what the EPD will actually paint. Host tests get a stub
  // that just delegates to the vendored Lookup() because they have no
  // LittleFS.
  if (pool_logos::HasResolvedLogo(name)) return {};
  if (name.empty()) return {};

  const auto sep = name.find_first_of(" \t_");
  if (sep == std::string::npos) return name;  // single-word; no split line
  const std::string left = name.substr(0, sep);
  std::size_t rhs = sep + 1;
  while (rhs < name.size() &&
         (name[rhs] == ' ' || name[rhs] == '\t' || name[rhs] == '_')) {
    ++rhs;
  }
  const std::string right = name.substr(rhs);
  if (right.empty()) return left;
  return left + "/" + right;
}

// Panel 0: pool logo (empty string) OR pool-name split-text fallback
// (single string, '/' between top/bottom halves — matches the v3
// getDisplayLabel convention "SATOSHI/RADIO"). Panels 1..N-2: digits
// right-justified. Panel N-1: unit suffix ("PH/S" etc.). Matches the v3
// single-panel label convention; previous v4 layout consumed two panels
// for the label which left-truncated "50.0K" to "0.0K" on 7-panel boards.
std::vector<std::string> BuildMiningPoolHashrate(const MiningPoolMirror& pool,
                                                 std::size_t n_panels) {
  std::vector<std::string> out(n_panels);
  if (n_panels == 0) return out;
  out[0] = PoolLabelCellFor(pool.name);
  if (n_panels < 3) return out;

  // Digit slots start at index 1 (after the single-panel label) and stop
  // before the trailing unit cell at N-1. One more slot than the previous
  // two-panel-label layout — earnings formatter's "50.0K" now fits.
  const std::size_t digit_slots = n_panels - 2;
  const std::size_t first_digit = 1;
  // Pass `digit_slots` as the max_chars so the hashrate value never
  // needs more digit cells than the board has.
  const MiningPoolHashrateLayout layout = LayoutMiningPoolHashrate(
      pool.hashrate, static_cast<unsigned int>(digit_slots ? digit_slots : 1));
  out[n_panels - 1] = layout.unit;

  const std::string& v = layout.value;
  if (v.empty()) return out;
  // Right-justify the formatted value across the digit slots. Cells that
  // are spaces become empty strings (matches CharSlot). On overflow the
  // leading chars are truncated — a pool reporting >digit_slots chars is
  // almost certainly a format we don't support; still, we emit *some*
  // digits rather than dropping the whole value.
  std::string s = v;
  if (s.size() > digit_slots) {
    s = s.substr(s.size() - digit_slots);
  }
  const std::size_t pad = digit_slots - s.size();
  for (std::size_t i = 0; i < digit_slots; ++i) {
    const char c = (i < pad) ? ' ' : s[i - pad];
    out[first_digit + i] = CharSlot(c);
  }
  return out;
}

// Scaled zap amount → short string ("21", "1.2k", "100M", "?"). Kept
// local to panel_texts so the header stays EPD-free. Must match the
// renderer-side FormatZapAmount in nostr_zap.cpp so the /api/status
// mirror agrees with what the panels paint. test_panel_texts pins the
// key cases.
std::string FormatZapAmountLocal(const std::optional<int64_t>& amount_sats,
                                 std::size_t max_int_cells) {
  if (!amount_sats || *amount_sats < 0) return "?";
  const int64_t v = *amount_sats;
  char int_buf[24];
  std::snprintf(int_buf, sizeof(int_buf), "%lld", static_cast<long long>(v));
  if (std::strlen(int_buf) <= max_int_cells) return int_buf;
  double x = static_cast<double>(v);
  const char* suffix;
  if (v >= 1'000'000'000LL) {
    x /= 1e9;
    suffix = "B";
  } else if (v >= 1'000'000LL) {
    x /= 1e6;
    suffix = "M";
  } else {
    x /= 1e3;
    suffix = "k";
  }
  char buf[16];
  if (x >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(x + 0.5), suffix);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f%s", x, suffix);
  }
  return buf;
}

std::vector<std::string> BuildNostrZap(
    const std::optional<int64_t>& amount_sats, bool use_sats_symbol,
    bool use_btc_symbol, std::size_t n_panels) {
  // Layout mirrors RenderNostrZapScreen in main/screens/nostr_zap.cpp:
  //   [ZAP][bolt][...blanks...][sats glyph][amount...]
  // Same on 7- and 8-panel boards — V8's extra cell widens the blank
  // gap rather than shifting ZAP/bolt rightward.
  // The bolt cell carries the mdi-lightning-bolt MDI glyph on the EPD;
  // the panel-text mirror represents it as an empty cell (same convention
  // bitaxe / mining-pool logo cells use — non-textual content). When
  // `use_sats_symbol=false` the glyph cell stays blank and the amount
  // uses one extra tail cell. Zapper message is intentionally not
  // mirrored; the snapshot field is kept so a future screen can surface
  // it without re-plumbing the relay listener.
  std::vector<std::string> out(n_panels);
  if (n_panels == 0) return out;
  const std::size_t zap_slot = 0;
  const std::size_t bolt_slot = zap_slot + 1;
  out[zap_slot] = "ZAP";
  if (n_panels <= bolt_slot + 1) return out;

  // Budget the integer formatter against the full tail (no glyph
  // reserve). When the integer overflows the tail, FormatZapAmountLocal
  // falls back to suffix; when it fits, the layout step below drops the
  // glyph so the digits aren't truncated.
  const std::size_t available_tail = n_panels - (bolt_slot + 1);
  const std::string amount = FormatZapAmountLocal(amount_sats, available_tail);
  std::size_t amount_cells = amount.size();
  if (amount_cells > available_tail) amount_cells = available_tail;
  if (amount_cells < 1) amount_cells = 1;
  const std::size_t first_amount = n_panels - amount_cells;
  const bool reserve_glyph = use_sats_symbol || use_btc_symbol;
  const bool has_glyph = reserve_glyph && first_amount > bolt_slot + 1;
  if (has_glyph) {
    out[first_amount - 1] =
        use_btc_symbol ? std::string(kPanelTextsBtcSignUtf8) : "STS";
  }

  // Right-justify the scaled amount into the tail cells. Truncate
  // leading chars only when the formatter overshoots (defensive — the
  // suffix path clamps to <= 4 chars).
  if (amount.size() >= amount_cells) {
    const std::size_t start = amount.size() - amount_cells;
    for (std::size_t i = 0; i < amount_cells; ++i) {
      out[first_amount + i].assign(1, amount[start + i]);
    }
  } else {
    const std::size_t pad = amount_cells - amount.size();
    for (std::size_t i = pad; i < amount_cells; ++i) {
      out[first_amount + i].assign(1, amount[i - pad]);
    }
  }
  return out;
}

std::vector<std::string> BuildMiningPoolEarnings(const MiningPoolMirror& pool,
                                                 std::size_t n_panels) {
  std::vector<std::string> out(n_panels);
  if (n_panels == 0) return out;
  out[0] = PoolLabelCellFor(pool.name);
  if (n_panels < 3) return out;

  // Digit slots start at index 1 (after the single-panel label) and stop
  // before the trailing unit cell at N-1. Matches the hashrate screen so
  // a switch between the two screens keeps the same column alignment.
  const std::size_t digit_slots = n_panels - 2;
  const std::size_t first_digit = 1;
  const MiningPoolEarningsLayout layout =
      LayoutMiningPoolEarnings(pool.daily_sats.value_or(-1));
  if (!layout.valid) {
    // Pool without a daily-earnings figure → blank digits, trailing
    // "SATS" placeholder. Keeps the mirror shape stable so the WebUI
    // still renders the row instead of going blank.
    out[n_panels - 1] = "SATS";
    return out;
  }
  out[n_panels - 1] = layout.unit_label;

  std::string s = layout.value;
  if (s.size() > digit_slots) s = s.substr(s.size() - digit_slots);
  const std::size_t pad = digit_slots - s.size();
  for (std::size_t i = 0; i < digit_slots; ++i) {
    const char c = (i < pad) ? ' ' : s[i - pad];
    out[first_digit + i] = CharSlot(c);
  }
  return out;
}

// Bitaxe panel-text mirror. Slot 0 is blank because the EPD renderer
// paints the vendored bitaxe logo bitmap (main/screens/assets/bitaxe_logo.cpp)
// on panel 0 — the bitmap has no textual representation, so the mirror
// leaves that cell empty and the WebUI is expected to render its own
// bitaxe glyph if it wants. Tail cells spread the value string one
// codepoint per panel, right-aligned across N-1 slots; OFFLINE fallback
// paints the word as separate cells.
std::vector<std::string> EmitBitaxeFrame(const std::string& value,
                                         std::size_t n_panels) {
  std::vector<std::string> out(n_panels);
  if (n_panels <= 1) return out;
  const std::size_t tail = n_panels - 1;
  std::vector<std::string> cells = SplitUtf8Codepoints(value);
  if (cells.size() < tail) {
    cells.insert(cells.begin(), tail - cells.size(), std::string(" "));
  } else if (cells.size() > tail) {
    cells.erase(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(
                                                   cells.size() - tail));
  }
  for (std::size_t i = 0; i < tail; ++i) {
    out[1 + i] = (cells[i] == " ") ? std::string() : cells[i];
  }
  return out;
}

std::vector<std::string> BuildBitaxeHashrate(const PanelTextInputs& in,
                                             std::size_t n_panels) {
  // Empty hostname === no sample yet. Mirror the renderer's OFFLINE
  // placeholder so the /api/status data[] stays truthy. OFFLINE still
  // spans the whole tail (including the would-be unit slot) — no
  // "/S" suffix when the device isn't reporting.
  if (in.bitaxe_hostname.empty() || !in.bitaxe_hashrate_ghs) {
    return EmitBitaxeFrame("OFFLINE", n_panels);
  }
  // Hashrate success: slot 0 blank (logo), slots 1..N-2 carry the
  // numeric value right-justified, slot N-1 carries the "<suffix>/S"
  // split-text cell. "GH/S" / "TH/S" / "PH/S" — same shape as the
  // mining-pool hashrate unit cell so the WebUI renders both with
  // DrawSplitText.
  std::vector<std::string> out(n_panels);
  if (n_panels <= 1) return out;
  const BitaxeHashrateParts parts =
      SplitBitaxeHashrate(*in.bitaxe_hashrate_ghs);
  if (n_panels < 3) {
    // Defensive: no room for a separate unit cell. Fall back to the
    // codepoint-per-slot tail so the mirror still round-trips.
    return EmitBitaxeFrame(parts.value + parts.suffix, n_panels);
  }
  out[n_panels - 1] = parts.suffix + "/S";
  const std::size_t digit_slots = n_panels - 2;
  std::vector<std::string> cells = SplitUtf8Codepoints(parts.value);
  if (cells.size() < digit_slots) {
    cells.insert(cells.begin(), digit_slots - cells.size(), std::string(" "));
  } else if (cells.size() > digit_slots) {
    cells.erase(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(
                                                   cells.size() - digit_slots));
  }
  for (std::size_t i = 0; i < digit_slots; ++i) {
    out[1 + i] = (cells[i] == " ") ? std::string() : cells[i];
  }
  return out;
}

std::vector<std::string> BuildBitaxeBestDiff(const PanelTextInputs& in,
                                             std::size_t n_panels) {
  if (in.bitaxe_hostname.empty() || !in.bitaxe_best_diff ||
      in.bitaxe_best_diff->empty()) {
    return EmitBitaxeFrame("OFFLINE", n_panels);
  }
  return EmitBitaxeFrame(*in.bitaxe_best_diff, n_panels);
}

std::vector<std::string> BuildClock(bool valid, int hour, int minute, int mday,
                                    int month, std::size_t n_panels,
                                    bool hide_lead_zero) {
  // Panel 0 = "dd/mm" date split-text (rendered via DrawSplitText in the
  // renderer; we mirror that with a "<dd>/<mm>" string). When SNTP
  // hasn't landed the renderer paints "-/-"; match that here so the
  // WebUI shows the same "not yet" state rather than a confusing empty.
  std::vector<std::string> out;
  out.reserve(n_panels);
  char label[24];
  if (valid && mday > 0 && month > 0 && mday < 100 && month < 100) {
    std::snprintf(label, sizeof(label), "%d/%d", mday, month);
  } else {
    std::snprintf(label, sizeof(label), "-/-");
  }
  out.emplace_back(label);
  const std::size_t digit_slots = n_panels - 1;
  const ClockLayout layout =
      ComputeClockLayout(valid, hour, minute, digit_slots, hide_lead_zero);
  AppendDigits(layout.digits, digit_slots, out);
  return out;
}

}  // namespace

std::vector<std::string> BuildPanelTexts(const PanelTextInputs& in,
                                         std::size_t n_panels) {
  if (n_panels == 0) return {};
  switch (in.kind) {
    case ScreenType::kBlockHeight:
      return BuildBlockHeight(in.block_height.value_or(0), n_panels);
    case ScreenType::kHalving:
      return BuildHalving(in.block_height.value_or(0), n_panels,
                          in.halving_as_blocks);
    case ScreenType::kBitcoinSupply:
      return BuildBitcoinSupply(in.block_height.value_or(0),
                                in.supply_big_chars, in.supply_percent,
                                n_panels);
    case ScreenType::kMarketCap:
      return BuildMarketCap(in.block_height.value_or(0), in.price, in.currency,
                            in.mcap_big_chars, in.share_dot, n_panels);
    case ScreenType::kMoscowTime:
      return BuildMoscowTime(in.currency, in.price, n_panels,
                             in.use_sats_symbol, in.use_btc_symbol,
                             in.use_mscw_time, in.share_dot);
    case ScreenType::kBtcPrice:
      return BuildBtcPrice(in.currency, in.price, n_panels, in.suffix_price,
                           in.mow_mode, in.share_dot);
    case ScreenType::kBlockFeeRate:
      return BuildFeeRate(in.block_fee_sats_vb, n_panels);
    case ScreenType::kClock:
      return BuildClock(in.clock_valid, in.hour, in.minute, in.mday, in.month,
                        n_panels, in.hide_lead_zero);
    case ScreenType::kMiningPoolHashrate:
      return BuildMiningPoolHashrate(in.pool, n_panels);
    case ScreenType::kMiningPoolEarnings:
      return BuildMiningPoolEarnings(in.pool, n_panels);
    case ScreenType::kBitaxeHashrate:
      return BuildBitaxeHashrate(in, n_panels);
    case ScreenType::kBitaxeBestDiff:
      return BuildBitaxeBestDiff(in, n_panels);
    case ScreenType::kCustom:
      // The custom screen's mirror is built directly from the pushed
      // cell strings (see ScreenManager::Render). BuildPanelTexts has no
      // view of those, so return all-empty — callers that end up here
      // (misconfigured wiring) see blanks rather than stale content.
      return std::vector<std::string>(n_panels);
    case ScreenType::kNostrZap:
      return BuildNostrZap(in.zap_amount_sats, in.use_sats_symbol,
                           in.use_btc_symbol, n_panels);
    case ScreenType::kDebug: {
      // Debug screen's layout is entirely markdown-driven and changes
      // per panel; no useful `data[]` mirror. Return a single label in
      // slot 0 so /api/status still round-trips non-empty.
      std::vector<std::string> out(n_panels);
      out[0] = "DEBUG";
      return out;
    }
    case ScreenType::kOtaUpdate: {
      // OTA overlay paints "UP/DATE" on every panel. /api/status
      // surfaces it as "UPDATE" across every slot so a watching client
      // sees a consistent mirror of what's on the EPDs.
      std::vector<std::string> out(n_panels, std::string("UPDATE"));
      return out;
    }
  }
  // Exhaustive switch above; fallback only on unreachable enum values.
  return std::vector<std::string>(n_panels);
}

}  // namespace btclock
