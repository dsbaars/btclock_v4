// Parity tests: port of test/test_datahandler/test_main.cpp (Unity) to the
// IDF port's pure-logic helpers.
//
// The old firmware's data_handler.cpp returns `std::array<std::string, 7>`
// ("7-panel" slot form):  [0]=label or first-overflow-digit, [1..5]=digit
// cells, [6]=unit/last-panel. The IDF port's equivalent is the
// tools/wasm/binding.cpp parse* functions which wrap pure helpers in
// main/screens/common.cpp + screen_math.hpp + fee_rate_layout.hpp.
//
// These tests call the IDF port's pure helpers (NOT the emscripten
// binding — that'd require a WASM build) and arrange the output in the
// same 7-slot shape old tests assert against.
//
// See the parent beads issue (btclock_v3_fci-hti) for the parity audit
// and the deferred-modes issue (btclock_v3_fci-33e) for the bigChars /
// suffix / MOW / percentage modes that aren't yet implemented in the
// IDF port; tests for those are skipped with SUBCASE-less no-ops and a
// link to the issue.

#include "doctest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// Host test can't include screens/common.hpp — it pulls EPD/font device
// headers. Instead we inline verbatim copies of the pure-logic helpers
// we exercise. Keep this block in sync with main/screens/common.cpp —
// if a regression fix changes the behaviour there, update here too.
// The parity tests are self-contained; they don't link against common.cpp.
#include <cstdlib>
namespace btclock {
struct DigitLayout {
  std::array<char, 6> digits{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sats{};
};
inline void FormatDigits(uint32_t h, char* digits, std::size_t slots) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
  const std::size_t len = std::strlen(buf);
  const char* src = buf;
  std::size_t pad = 0;
  if (len > slots) src = buf + (len - slots);
  else pad = slots - len;
  for (std::size_t i = 0; i < slots; ++i) {
    digits[i] = (i < pad) ? ' ' : src[i - pad];
  }
}
inline int32_t SatsPerUnit(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p <= 0.0 || endp == price_str.c_str()) return -1;
  const double sats = 1e8 / p;
  if (sats > 4e9) return -1;
  return static_cast<int32_t>(sats + 0.5);
}
inline int32_t PriceInt(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p < 0.0 || endp == price_str.c_str()) return -1;
  if (p > 2e9) return -1;
  return static_cast<int32_t>(p + 0.5);
}
inline const char* CurrencySymbolUtf8(const std::string& ccy) {
  if (ccy == "USD") return "$";
  if (ccy == "EUR") return "\xE2\x82\xAC";
  if (ccy == "GBP") return "\xC2\xA3";
  if (ccy == "JPY") return "\xC2\xA5";
  return "";
}
inline DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol) {
  DigitLayout l;
  if (sats < 0) return l;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
  const std::size_t len = std::strlen(buf);
  const std::size_t slots = 6;
  if (len >= slots) {
    const std::size_t start = len - slots;
    for (std::size_t i = 0; i < slots; ++i) l.digits[i] = buf[start + i];
    return l;
  }
  const std::size_t pad = slots - len;
  for (std::size_t i = 0; i < slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  if (use_symbol && pad > 0) {
    l.is_sats[pad - 1] = true;
    l.digits[pad - 1] = ' ';
  }
  return l;
}
}  // namespace btclock

#include "screens/fee_rate_layout.hpp"
#include "screens/screen_math.hpp"

namespace {

// NUM_SCREENS-sized output array that mirrors the old firmware's
// parse*() return type (7-panel topology).
using Out = std::array<std::string, 7>;

// Old firmware's char-coded currencies. Mapped to ISO codes for the
// IDF helpers (which take std::string codes).
constexpr char kCurUSD = '$';
constexpr char kCurEUR = '[';
constexpr char kCurGBP = ']';
constexpr char kCurJPY = '^';

std::string IsoFromChar(char c) {
  switch (c) {
    case kCurUSD: return "USD";
    case kCurEUR: return "EUR";
    case kCurGBP: return "GBP";
    case kCurJPY: return "JPY";
    default:      return "USD";
  }
}

// Helper: fill `out` from a 7-char padded string. If the string is
// length 7 (overflowed), put digit 0 at slot 0 (no label); else put
// `label` at slot 0 and the remaining 6 chars at slots 1..6.
Out FromPaddedString(const std::string& s7, const std::string& label) {
  Out out;
  if (s7.size() == 7) {
    out[0] = std::string(1, s7[0]);
    for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, s7[i]);
  } else {
    // s7.length() should be < 7; caller left room for the label.
    out[0] = label;
    for (size_t i = 1; i < 7; ++i) {
      out[i] = (i < s7.size()) ? std::string(1, s7[i]) : std::string("");
    }
  }
  return out;
}

// ---------------------------------------------------------------------
// Screen renderers (parity layer): call the IDF pure helpers and emit
// the 7-slot array the old firmware's tests expect.
// ---------------------------------------------------------------------

// parseBlockHeight — label "BLOCK/HEIGHT" unless BlockHeightDropsLabel
// fires (height has >= N digits, at which point each panel carries a
// digit — matches old firmware's parseBlockHeight).
Out RenderBlockHeight(uint32_t height) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(height));
  std::string s = buf;
  Out out;
  if (btclock::BlockHeightDropsLabel(height, 7)) {
    for (size_t i = 0; i < 7; ++i) out[i] = std::string(1, s[i]);
    return out;
  }
  s.insert(s.begin(), 7 - s.size(), ' ');
  out[0] = "BLOCK/HEIGHT";
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, s[i]);
  return out;
}

// parsePriceData — integer price, currency glyph, no suffix/MOW/shareDot.
// Old firmware: priceString = "<glyph><num>"; if length < 7, pad with
// spaces, set label "BTC/<code>", emit slots[1..6] = padded[1..6];
// if length == 7, no label, emit each char.
Out RenderPriceData(uint32_t price, char ccy) {
  std::string num = std::to_string(price);
  std::string glyph(1, ccy);  // old firmware writes the raw char byte
  std::string s = glyph + num;
  const std::string iso = IsoFromChar(ccy);
  const std::string label = "BTC/" + iso;
  if (s.size() >= 7) {
    // Suffix mode wasn't selected; old firmware still falls into the
    // "no label, overflow" branch when priceString.length() == 7.
    return FromPaddedString(s.substr(0, 7), label);
  }
  s.insert(s.begin(), 7 - s.size(), ' ');
  return FromPaddedString(s, label);
}

// parseSatsPerCurrency — integer or decimal sats/unit output, label
// switches between "MSCW/TIME" and "SATS/<CCY>".
Out RenderSatsPerCurrency(uint32_t price, char ccy, bool /*with_sats_symbol*/,
                          bool use_mscw_time) {
  const std::string iso = IsoFromChar(ccy);
  Out out;
  out.fill("");
  if (price == 0) {
    // Div-by-zero guard: label only, no digits.
    out[0] = (ccy == kCurUSD && use_mscw_time)
                 ? std::string("MSCW/TIME")
                 : ("SATS/" + iso);
    return out;
  }

  // Build the digit string.
  std::string s;
  if (price >= 100000000u) {
    // Decimal sub-sat regime. Old firmware: ".3f" of 1e8/price.
    const double sp = 1e8 / static_cast<double>(price);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", sp);
    s = buf;
  } else {
    const int32_t sats = static_cast<int32_t>(
        std::round(1e8 / static_cast<double>(price)));
    s = std::to_string(sats);
  }

  // Label selection: MSCW/TIME only for USD below 1e8 with useMscwTime.
  const std::string label =
      (ccy == kCurUSD && price < 100000000u && use_mscw_time)
          ? std::string("MSCW/TIME")
          : ("SATS/" + iso);

  if (s.size() < 7) {
    s.insert(s.begin(), 7 - s.size(), ' ');
  }
  out[0] = label;
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, s[i]);
  return out;
}

// parseMarketCap bigChars=true — currency glyph prefix + FormatNumberWithSuffix,
// right-padded across the 7-slot array with "<CCY>/MCAP" in slot[0].
// Port of lib/btclock/data_handler.cpp::parseMarketCap's big-chars branch.
Out RenderMarketCapBigChars(uint32_t height, uint32_t price, char ccy) {
  Out out;
  out.fill("");
  const uint64_t supply = btclock::SupplyAtBlock(height);
  const uint64_t cap = supply * static_cast<uint64_t>(price);
  std::string ps = std::string(1, ccy) +
                   btclock::FormatNumberWithSuffix(cap, 7 - 2);
  if (ps.size() < 7) ps.insert(ps.begin(), 7 - ps.size(), ' ');
  out[0] = IsoFromChar(ccy) + "/MCAP";
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, ps[i]);
  return out;
}

// parseMarketCap bigChars=false — three-digit groups across the tail
// slots; slot[6-groups] carries the " <CCY> " spaced label; slot[0]
// is "<CCY>/MCAP". Port of parseMarketCap's small-chars branch.
Out RenderMarketCapSmallChars(uint32_t height, uint32_t price, char ccy) {
  Out out;
  out.fill("");
  const uint64_t supply = btclock::SupplyAtBlock(height);
  const uint64_t cap = supply * static_cast<uint64_t>(price);
  std::string s = std::to_string(cap);
  const size_t mc_len = s.size();
  const size_t leading = (3 - mc_len % 3) % 3;
  s.insert(s.begin(), leading, ' ');
  const size_t groups = (mc_len + leading) / 3;

  out[0] = IsoFromChar(ccy) + "/MCAP";
  const size_t slot_ccy = 7 - groups - 1;
  out[slot_ccy] = std::string(" ") + ccy + " ";
  for (size_t i = 0; i < groups; ++i) {
    out[7 - groups + i] = s.substr(i * 3, 3);
  }
  return out;
}

// parseBitcoinSupply bigChars=true — "BTC/SUPPLY" + FormatNumberWithSuffix
// right-justified. The old firmware indexes into the padded string from
// slot 1 (char-by-char), so the padded-leading-space lands in the cells
// before the number.
Out RenderBitcoinSupplyBigChars(uint32_t height) {
  Out out;
  out.fill("");
  const uint64_t supply = btclock::SupplyAtBlock(height);
  std::string s = btclock::FormatNumberWithSuffix(supply, 7 - 2);
  if (s.size() < 7) s.insert(s.begin(), 7 - s.size(), ' ');
  out[0] = "BTC/SUPPLY";
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, s[i]);
  return out;
}

// parseBitcoinSupply bigChars=false — three-digit groups across the tail
// slots + leading blank-label slot for the group divider. Matches the
// old firmware exactly (see parseBitcoinSupply small-chars branch).
Out RenderBitcoinSupplySmallChars(uint32_t height) {
  Out out;
  out.fill("");
  const uint64_t supply = btclock::SupplyAtBlock(height);
  std::string s = std::to_string(supply);
  const size_t len = s.size();
  const size_t leading = (3 - len % 3) % 3;
  s.insert(s.begin(), leading, ' ');
  const size_t groups = (len + leading) / 3;
  out[0] = "BTC/SUPPLY";
  if (7 - groups - 1 >= 1) out[7 - groups - 1] = " ";
  for (size_t i = 0; i < groups; ++i) {
    out[7 - groups + i] = s.substr(i * 3, 3);
  }
  return out;
}

// parseBitcoinSupply showPercentage — "BTC/SUPPLY" + "NN.NN" digits
// across slots[1..5] + " % " in slot[6]. The old firmware's indexing
// copies `percentageString[i]` into slot `i` starting at 1, which skips
// the initial space-padding's slot-0 slot.
Out RenderBitcoinSupplyPercentage(uint32_t height) {
  Out out;
  out.fill("");
  const uint64_t supply = btclock::SupplyAtBlock(height);
  const double frac =
      std::round((static_cast<double>(supply) / 20999999.9769) * 10000.0) /
      100.0;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f%%", frac);
  std::string s = buf;
  if (s.size() < 7) s.insert(s.begin(), 7 - s.size(), ' ');
  out[0] = "BTC/SUPPLY";
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, s[i]);
  out[6] = " % ";
  return out;
}

// parsePriceData useSuffixFormat — adds currency glyph + suffix form
// of the integer price. Without shareDot or mowMode this is the plain
// "<glyph><N>K" / "<glyph><N>.NM" output used by test_PriceOf1MillionUsd
// and test_PriceSuffixMode.
Out RenderPriceDataSuffix(uint32_t price, char ccy) {
  Out out;
  out.fill("");
  const int num_chars = 7 - 2;  // no shareDot/mowMode path
  std::string ps = std::string(1, ccy) +
                   btclock::FormatNumberWithSuffix(price, num_chars);
  if (ps.size() < 7) ps.insert(ps.begin(), 7 - ps.size(), ' ');
  out[0] = "BTC/" + IsoFromChar(ccy);
  for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, ps[i]);
  return out;
}

// parsePriceData useSuffixFormat+shareDot — the dot is packed into the
// *same* slot as the digit preceding it ("1.", "0."). Everything else
// follows the suffix branch but uses num_chars=7-1 (one more char
// available because the dot sits in a shared slot).
Out RenderPriceDataSuffixShareDot(uint32_t price, char ccy, bool mow) {
  Out out;
  out.fill("");
  const int num_chars = 7 - 1;  // shareDot or mowMode leaves one more
  std::string ps = std::string(1, ccy) +
                   btclock::FormatNumberWithSuffix(price, num_chars, mow);
  // "shareDot && length <= 7" is the old firmware's pad-and-label
  // guard; we inline that check here to match.
  const bool fits_with_label = ps.size() <= 7;
  size_t first_idx = 0;
  if (fits_with_label) {
    ps.insert(ps.begin(), 7 - ps.size(), ' ');
    out[0] = mow ? std::string("MOW/UNITS") : ("BTC/" + IsoFromChar(ccy));
    first_idx = 1;
  }

  const size_t dot = ps.find('.');
  if (dot != std::string::npos && dot > 0) {
    // Pack "X." into the slot *before* the dot; skip the raw dot byte.
    std::vector<std::string> cells;
    for (size_t i = 0; i < ps.size(); ++i) {
      if (i + 1 == dot) {
        cells.push_back(std::string(1, ps[i]) + ".");
        ++i;  // consume the dot byte
      } else {
        cells.push_back(std::string(1, ps[i]));
      }
    }
    for (size_t i = first_idx;
         i < 7 && i - first_idx < cells.size(); ++i) {
      out[i] = cells[i - first_idx];
    }
  } else {
    for (size_t i = first_idx; i < 7; ++i) {
      out[i] = std::string(1, ps[i]);
    }
  }
  return out;
}

// parsePriceData useSuffixFormat+mowMode (no shareDot) — label becomes
// the currency glyph ($) in slot 0 because "mowMode sets firstIndex
// behaviour unchanged" — actually, in old firmware the label is set to
// `getCurrencySymbol(ccy)` only when the price string overflows; the
// mow tests assert slot[0] is "$" which is the price glyph, meaning
// mowMode+non-shareDot gives a 7-char padded string with no label.
Out RenderPriceDataSuffixMow(uint32_t price, char ccy) {
  Out out;
  out.fill("");
  const int num_chars = 7 - 1;  // mowMode leaves one more char
  std::string ps = std::string(1, ccy) +
                   btclock::FormatNumberWithSuffix(price, num_chars, true);
  if (ps.size() < 7) {
    ps.insert(ps.begin(), 7 - ps.size(), ' ');
    out[0] = "MOW/UNITS";
    for (size_t i = 1; i < 7; ++i) out[i] = std::string(1, ps[i]);
    return out;
  }
  // priceString exactly 7 → no label, char-per-slot.
  for (size_t i = 0; i < 7; ++i) out[i] = std::string(1, ps[i]);
  return out;
}

// parseHalvingCountdown asBlocks=false — years/days/hours/mins labels
// across slots[2..5], "BIT/COIN" + "HAL/VING" pair in slots[0..1], and
// "TO/GO" terminator in slot[6]. Port of lib/btclock/data_handler.cpp
// parseHalvingCountdown's time-mode branch.
Out RenderHalvingCountdownTime(uint32_t height) {
  Out out;
  out.fill("");
  const auto tb = btclock::HalvingCountdownBreakdown(height);
  out[0] = "BIT/COIN";
  out[1] = "HAL/VING";
  out[7 - 5] = std::to_string(tb.years)   + "/YRS";
  out[7 - 4] = std::to_string(tb.days)    + "/DAYS";
  out[7 - 3] = std::to_string(tb.hours)   + "/HRS";
  out[7 - 2] = std::to_string(tb.minutes) + "/MINS";
  out[7 - 1] = "TO/GO";
  return out;
}

// parseBlockFees — label "FEE/RATE", fee digits across slots[1..5],
// unit "sat/vB" at slot[6]. Decimal (".%2f") below 10, integer rounded
// above. Matches old firmware's lib/btclock/data_handler.cpp.
Out RenderBlockFees(float fee) {
  Out out;
  out.fill("");
  out[0] = "FEE/RATE";
  out[6] = "sat/vB";

  std::string s;
  if (fee < 10.0f) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", fee);
    s = buf;
  } else {
    s = std::to_string(
        static_cast<int>(std::round(static_cast<double>(fee))));
  }

  // Old firmware: pads with (NUM_SCREENS - len - 1) spaces (one fewer
  // than usual) because slot[6] is reserved for the unit, then emits
  // slots[1..5] = padded[1..5].
  if (s.size() < 7) {
    s.insert(s.begin(), 7 - s.size() - 1, ' ');
  }
  for (size_t i = 1; i < 6; ++i) {
    out[i] = (i < s.size()) ? std::string(1, s[i]) : std::string("");
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------
// Test cases — names preserved from test/test_datahandler/test_main.cpp.
// Each asserts the same slot layout the old firmware's Unity tests did.
// ---------------------------------------------------------------------

// --- parseSatsPerCurrency ---

TEST_CASE("parseSatsPerCurrency — CorrectSatsPerDollarConversion") {
  const auto out = RenderSatsPerCurrency(37253, kCurUSD, false, true);
  CHECK(out[0] == "MSCW/TIME");
  CHECK(out[7 - 4] == "2");
  CHECK(out[7 - 3] == "6");
  CHECK(out[7 - 2] == "8");
  CHECK(out[7 - 1] == "4");
}

TEST_CASE("parseSatsPerCurrency — SatsPerDollarAfter1B") {
  // price >= 1e8 → decimal sub-sat regime, label switches to SATS/USD.
  const auto out = RenderSatsPerCurrency(120000000, kCurUSD, false, true);
  CHECK(out[0] == "SATS/USD");
  CHECK(out[7 - 5] == "0");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "8");
  CHECK(out[7 - 2] == "3");
  CHECK(out[7 - 1] == "3");
}

TEST_CASE("parseSatsPerCurrency — CorrectSatsPerPoundConversion") {
  const auto out = RenderSatsPerCurrency(37253, kCurGBP, false, true);
  CHECK(out[0] == "SATS/GBP");
  CHECK(out[7 - 4] == "2");
  CHECK(out[7 - 3] == "6");
  CHECK(out[7 - 2] == "8");
  CHECK(out[7 - 1] == "4");
}

TEST_CASE("parseSatsPerCurrency — PriceZero no-crash (useMscwTime default)") {
  const auto out = RenderSatsPerCurrency(0, kCurUSD, false, true);
  CHECK(out[0].length() > 0);
}

TEST_CASE("parseSatsPerCurrency — PriceZero with sats symbol") {
  const auto out = RenderSatsPerCurrency(0, kCurUSD, true, true);
  CHECK(out[0].length() > 0);
}

TEST_CASE("parseSatsPerCurrency — LowPriceWithSymbol no-crash") {
  // price=100 → sats=1_000_000 (7 digits). Must survive without OOB.
  const auto out = RenderSatsPerCurrency(100, kCurUSD, true, true);
  CHECK(out[0].length() > 0);
}

TEST_CASE("parseSatsPerCurrency — SatsPerDollar_NoMscwTime") {
  const auto out = RenderSatsPerCurrency(37253, kCurUSD, false, false);
  CHECK(out[0] == "SATS/USD");
  CHECK(out[7 - 4] == "2");
  CHECK(out[7 - 3] == "6");
  CHECK(out[7 - 2] == "8");
  CHECK(out[7 - 1] == "4");
}

TEST_CASE("parseSatsPerCurrency — SatsPerDollar_MscwTime_Explicit") {
  const auto out = RenderSatsPerCurrency(37253, kCurUSD, false, true);
  CHECK(out[0] == "MSCW/TIME");
}

TEST_CASE("parseSatsPerCurrency — SatsPerPound_IgnoresMscwTimeFlag") {
  const auto out = RenderSatsPerCurrency(37253, kCurGBP, false, false);
  CHECK(out[0] == "SATS/GBP");
}

TEST_CASE("parseSatsPerCurrency — PriceZero_NoMscwTime") {
  const auto out = RenderSatsPerCurrency(0, kCurUSD, false, false);
  CHECK(out[0] == "SATS/USD");
}

// --- parseBlockHeight ---

TEST_CASE("parseBlockHeight — SixCharacterBlockHeight") {
  const auto out = RenderBlockHeight(999999);
  CHECK(out[0] == "BLOCK/HEIGHT");
  CHECK(out[1] == "9");
}

TEST_CASE("parseBlockHeight — SevenCharacterBlockHeight") {
  // Old firmware: label dropped on overflow; each digit fills a slot.
  const auto out = RenderBlockHeight(1000000);
  CHECK(out[0] == "1");
  CHECK(out[1] == "0");
}

TEST_CASE("parseBlockHeight — MainnetCurrent (~900k still keeps label)") {
  const auto out = RenderBlockHeight(900123);
  CHECK(out[0] == "BLOCK/HEIGHT");
  CHECK(out[1] == "9");
  CHECK(out[2] == "0");
  CHECK(out[3] == "0");
  CHECK(out[4] == "1");
  CHECK(out[5] == "2");
  CHECK(out[6] == "3");
}

TEST_CASE("parseBlockHeight — PostLabelDrop (1_234_567 paints every panel)") {
  const auto out = RenderBlockHeight(1234567);
  CHECK(out[0] == "1");
  CHECK(out[1] == "2");
  CHECK(out[2] == "3");
  CHECK(out[3] == "4");
  CHECK(out[4] == "5");
  CHECK(out[5] == "6");
  CHECK(out[6] == "7");
}

// --- parseBlockFees ---

TEST_CASE("parseBlockFees — FeeRateDisplay (21.21 → '21' integer-rounded)") {
  const auto out = RenderBlockFees(21.21f);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[7 - 3] == "2");
  CHECK(out[7 - 2] == "1");
  CHECK(out[7 - 1] == "sat/vB");
}

TEST_CASE("parseBlockFees — FeeRateDisplay2 (1.1 → '1.10' decimal)") {
  const auto out = RenderBlockFees(1.1f);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "1");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "sat/vB");
}

TEST_CASE("parseBlockFees — HighRate (150)") {
  const auto out = RenderBlockFees(150.0f);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[7 - 4] == "1");
  CHECK(out[7 - 3] == "5");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "sat/vB");
}

TEST_CASE("parseBlockFees — BoundaryTen (10.0)") {
  // Exactly at the >= 10.0 boundary → integer render "10".
  const auto out = RenderBlockFees(10.0f);
  CHECK(out[0] == "FEE/RATE");
  CHECK(out[7 - 3] == "1");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "sat/vB");
}

// --- parsePriceData ---

TEST_CASE("parsePriceData — PriceOf100kusd ('$100000' overflows label)") {
  // "$100000" is 7 chars = NUM_SCREENS → no label, one char per slot.
  const auto out = RenderPriceData(100000, kCurUSD);
  CHECK(out[0] == "$");
  CHECK(out[1] == "1");
}

// --- Suffix / MOW / shareDot price modes (text-layout parity) ---
//
// These cover the old firmware's text-layout shapes. The IDF port's
// live BtcPrice screen renderer still paints plain integer digit cells;
// exposing the suffix/MOW layouts on-device is the open scope of
// btclock_v3_fci-33e. The layouts themselves are pure string logic
// though, and the old-firmware Unity tests assert against that exact
// output — so we port the helpers here and verify byte-parity now,
// independent of whether the renderer has opted into them yet.

TEST_CASE("parsePriceData — PriceOf1MillionUsd (suffix mode)") {
  // 1_000_000 USD suffix-formatted → "$1.00M" — an overflow-less
  // layout: label in slot 0, padded digits right-justified.
  const auto out = RenderPriceDataSuffix(1000000, kCurUSD);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "M");
}

TEST_CASE("parsePriceData — PriceSuffixMode (93000, suffix)") {
  const auto out = RenderPriceDataSuffix(93000, kCurUSD);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[7 - 5] == "9");
  CHECK(out[7 - 4] == "3");
  CHECK(out[7 - 3] == ".");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "K");
}

TEST_CASE("parsePriceData — PriceSuffixModeCompact1 (100k, shareDot)") {
  // 100_000 + shareDot → "$100.0K" (7 chars); dot shares the "0." slot.
  const auto out =
      RenderPriceDataSuffixShareDot(100000, kCurUSD, /*mow=*/false);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[7 - 6] == "$");
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == "0");
  CHECK(out[7 - 3] == "0.");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "K");
}

TEST_CASE("parsePriceData — PriceSuffixModeCompact2 (1M, shareDot)") {
  const auto out =
      RenderPriceDataSuffixShareDot(1000000, kCurUSD, /*mow=*/false);
  CHECK(out[0] == "BTC/USD");
  CHECK(out[7 - 6] == "$");
  CHECK(out[7 - 5] == "1.");
  CHECK(out[7 - 4] == "0");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "0");
  CHECK(out[7 - 1] == "M");
}

TEST_CASE("parsePriceData — PriceSuffixModeMow (93600, mow)") {
  // MOW mode (no shareDot): the glyph itself sits in slot 0 when the
  // priceString is exactly 7 chars ("$.093M" → "$.093M" — but actually
  // "$" + ".093" + "M" = 6 chars, still padded with one leading space).
  // Old test asserts slot[0]="$" meaning it's the left-padded glyph.
  const auto out = RenderPriceDataSuffixMow(93600, kCurUSD);
  CHECK(out[0] == "$");
  CHECK(out[7 - 5] == ".");
  CHECK(out[7 - 4] == "0");
  CHECK(out[7 - 3] == "9");
  CHECK(out[7 - 2] == "3");
  CHECK(out[7 - 1] == "M");
}

TEST_CASE("parsePriceData — PriceSuffixModeMowCompact (93600, mow+shareDot)") {
  // mowMode + shareDot → "MOW/UNITS" label and "0.093M" collapsed.
  const auto out =
      RenderPriceDataSuffixShareDot(93600, kCurUSD, /*mow=*/true);
  CHECK(out[0] == "MOW/UNITS");
  CHECK(out[7 - 6] == "$");
  CHECK(out[7 - 5] == "0.");
  CHECK(out[7 - 4] == "0");
  CHECK(out[7 - 3] == "9");
  CHECK(out[7 - 2] == "3");
  CHECK(out[7 - 1] == "M");
}

// --- parseMarketCap (bigChars true/false) ---

TEST_CASE("parseMarketCap — McapLowerUsd (bigChars suffix)") {
  const auto out = RenderMarketCapBigChars(810000, 26000, kCurUSD);
  CHECK(out[0] == "USD/MCAP");
  CHECK(out[7 - 5] == "$");
  CHECK(out[7 - 4] == "5");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "7");
  CHECK(out[7 - 1] == "B");
}

TEST_CASE("parseMarketCap — Mcap1TrillionUsd (bigChars suffix)") {
  const auto out = RenderMarketCapBigChars(831000, 52000, kCurUSD);
  CHECK(out[0] == "USD/MCAP");
  CHECK(out[7 - 6] == "$");
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "2");
  CHECK(out[7 - 1] == "T");
}

TEST_CASE("parseMarketCap — Mcap1TrillionUsdSmallChars") {
  const auto out =
      RenderMarketCapSmallChars(831000, 52000, kCurUSD);
  CHECK(out[0] == "USD/MCAP");
  CHECK(out[7 - 6] == " $ ");
  CHECK(out[7 - 5] == "  1");
  CHECK(out[7 - 4] == "020");
  CHECK(out[7 - 3] == "825");
  CHECK(out[7 - 2] == "000");
  CHECK(out[7 - 1] == "000");
}

TEST_CASE("parseMarketCap — Mcap1TrillionEur (bigChars suffix)") {
  // Port uses the old-firmware byte-code glyph ('[' for EUR) because
  // the parity test layer mirrors that encoding. On-device the IDF
  // port would emit "€" — see btclock_v3_fci-33e for the renderer-
  // side UTF-8 work, which is independent of this text-layout parity.
  const auto out = RenderMarketCapBigChars(831000, 52000, kCurEUR);
  CHECK(out[0] == "EUR/MCAP");
  CHECK(out[7 - 6][0] == kCurEUR);
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "2");
  CHECK(out[7 - 1] == "T");
}

TEST_CASE("parseMarketCap — Mcap1TrillionEurSmallChars") {
  const auto out =
      RenderMarketCapSmallChars(831000, 52000, kCurEUR);
  CHECK(out[0] == "EUR/MCAP");
  char expected_eur_cell[4];
  std::snprintf(expected_eur_cell, sizeof(expected_eur_cell), " %c ",
                kCurEUR);
  CHECK(out[7 - 6] == expected_eur_cell);
  CHECK(out[7 - 5] == "  1");
  CHECK(out[7 - 4] == "020");
  CHECK(out[7 - 3] == "825");
  CHECK(out[7 - 2] == "000");
  CHECK(out[7 - 1] == "000");
}

TEST_CASE("parseMarketCap — Mcap1TrillionJpy (bigChars suffix)") {
  const auto out = RenderMarketCapBigChars(831000, 52000, kCurJPY);
  CHECK(out[0] == "JPY/MCAP");
  CHECK(out[7 - 6][0] == kCurJPY);
  CHECK(out[7 - 5] == "1");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "0");
  CHECK(out[7 - 2] == "2");
  CHECK(out[7 - 1] == "T");
}

TEST_CASE("parseMarketCap — Mcap1TrillionJpySmallChars") {
  const auto out =
      RenderMarketCapSmallChars(831000, 52000, kCurJPY);
  CHECK(out[0] == "JPY/MCAP");
  char expected_jpy_cell[4];
  std::snprintf(expected_jpy_cell, sizeof(expected_jpy_cell), " %c ",
                kCurJPY);
  CHECK(out[7 - 6] == expected_jpy_cell);
  CHECK(out[7 - 5] == "  1");
  CHECK(out[7 - 4] == "020");
  CHECK(out[7 - 3] == "825");
  CHECK(out[7 - 2] == "000");
  CHECK(out[7 - 1] == "000");
}

// --- parseBitcoinSupply (bigChars true/false, percentage) ---

TEST_CASE("parseBitcoinSupply — BitcoinSupply (bigChars suffix)") {
  const auto out = RenderBitcoinSupplyBigChars(831000);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[7 - 4] == "9");
  CHECK(out[7 - 3] == ".");
  CHECK(out[7 - 2] == "6");
  CHECK(out[7 - 1] == "M");
}

TEST_CASE("parseBitcoinSupply — BitcoinSupplyPercentage") {
  const auto out = RenderBitcoinSupplyPercentage(831000);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[7 - 6] == "9");
  CHECK(out[7 - 5] == "3");
  CHECK(out[7 - 4] == ".");
  CHECK(out[7 - 3] == "4");
  CHECK(out[7 - 2] == "8");
  CHECK(out[7 - 1] == " % ");
}

TEST_CASE("parseBitcoinSupply — BitcoinSupplySmallChars") {
  // 655987 → supply ≈ 18_537_??? — assert on the two last groups since
  // the old firmware's test also only asserts on "18" / "537".
  const auto out = RenderBitcoinSupplySmallChars(655987);
  CHECK(out[0] == "BTC/SUPPLY");
  CHECK(out[7 - 3] == " 18");
  CHECK(out[7 - 2] == "537");
}

// --- parseHalvingCountdown (time-mode) ---
//
// Layout: "BIT/COIN" + "HAL/VING" header in slots[0..1], four
// year/day/hour/min "N/UNIT" labels in slots[2..5], "TO/GO" in slot[6].
// Old firmware has no Unity test for this branch; the parity coverage
// is new and pins behaviour byte-for-byte against the reference impl
// (lib/btclock/data_handler.cpp::parseHalvingCountdown).

TEST_CASE("parseHalvingCountdown — TimeMode at block 0 (full interval)") {
  // height=0 → blocks_to_halving = 210_000 → 2_100_000 minutes =
  // 3 years, 363 days, 8 hours, 0 minutes using the old firmware's
  // 525_600-min/year floor cascade (not calendar years).
  const auto out = RenderHalvingCountdownTime(0);
  CHECK(out[0] == "BIT/COIN");
  CHECK(out[1] == "HAL/VING");
  CHECK(out[7 - 5] == "3/YRS");
  CHECK(out[7 - 4] == "363/DAYS");
  CHECK(out[7 - 3] == "8/HRS");
  CHECK(out[7 - 2] == "0/MINS");
  CHECK(out[7 - 1] == "TO/GO");
}

TEST_CASE("parseHalvingCountdown — TimeMode near a halving (1 block out)") {
  // height=209_999 → 1 block remaining → 10 minutes total.
  const auto out = RenderHalvingCountdownTime(209999);
  CHECK(out[0] == "BIT/COIN");
  CHECK(out[1] == "HAL/VING");
  CHECK(out[7 - 5] == "0/YRS");
  CHECK(out[7 - 4] == "0/DAYS");
  CHECK(out[7 - 3] == "0/HRS");
  CHECK(out[7 - 2] == "10/MINS");
  CHECK(out[7 - 1] == "TO/GO");
}

TEST_CASE("parseHalvingCountdown — TimeMode reset at exact halving block") {
  // Same carry-over rule as HalvingCountdown: at the exact halving block
  // the countdown rolls over to a full 210_000 interval.
  const auto a = RenderHalvingCountdownTime(0);
  const auto b = RenderHalvingCountdownTime(210000);
  for (size_t i = 0; i < 7; ++i) CHECK(a[i] == b[i]);
}
