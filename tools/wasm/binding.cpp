// Emscripten bindings over the ESP-IDF port's screen-layout helpers.
//
// Each parse* function returns a 7-element array: panel 0 = label text,
// panels 1..6 = per-digit-panel content (single char, or UTF-8 symbol
// for the currency / sats slots). Matches the on-device panel topology
// so the HTML preview can mimic the e-paper rendering without pulling
// in the EPD driver.
//
// Build: compiled together with main/screens/common.cpp. The full
// main/screens/common.hpp is NOT included here — it would transitively
// pull ESP-IDF-only headers (epd_ssd1680.hpp, font.hpp). Instead the
// pure-logic helpers are forward-declared below.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "screens/fee_rate_layout.hpp"

namespace btclock {

struct DigitLayout {
  std::array<char, 6> digits{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sats{};
};

void FormatDigits(uint32_t h, char* digits, std::size_t slots);
int32_t PriceInt(const std::string& price_str);
int32_t SatsPerUnit(const std::string& price_str);
const char* CurrencySymbolUtf8(const std::string& ccy);
DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol);

}

using emscripten::val;

namespace {

val DigitsToArray(const char* label, const std::array<char, 6>& d) {
  val a = val::array();
  a.set(0, std::string(label));
  for (int i = 0; i < 6; ++i) {
    char buf[2] = {d[i], '\0'};
    a.set(i + 1, std::string(buf));
  }
  return a;
}

// parseBlockHeight — "BLOCK/HEIGHT" + right-justified integer digits.
val parseBlockHeight(int block_height) {
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0) {
    btclock::FormatDigits(static_cast<uint32_t>(block_height), d.data(), 6);
  }
  return DigitsToArray("BLOCK/HEIGHT", d);
}

// parsePriceData — "BTC/<CCY>" label + currency-symbol panel one slot
// before the first digit (if a glyph exists and the value leaves a gap).
val parsePriceData(int price_int, std::string currency) {
  const std::string label = "BTC/" + currency;
  const char* symbol_utf8 = btclock::CurrencySymbolUtf8(currency);
  const bool use_symbol = symbol_utf8[0] != '\0';

  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sym{};
  const int32_t vv = price_int < 0 ? 0 : price_int;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", vv);
  const std::size_t len = std::strlen(buf);
  if (len >= 6) {
    for (std::size_t i = 0; i < 6; ++i) d[i] = buf[len - 6 + i];
  } else {
    const std::size_t pad = 6 - len;
    for (std::size_t i = pad; i < 6; ++i) d[i] = buf[i - pad];
    if (use_symbol && pad > 0) is_sym[pad - 1] = true;
  }

  val a = val::array();
  a.set(0, label);
  for (int i = 0; i < 6; ++i) {
    if (is_sym[i]) {
      a.set(i + 1, std::string(symbol_utf8));
    } else {
      char cell[2] = {d[i], '\0'};
      a.set(i + 1, std::string(cell));
    }
  }
  return a;
}

// parseSatsPerCurrency — either "MSCW/TIME" (USD, classic Moscow-time
// range) or "SATS/<CCY>", followed by SatsPerUnit laid out right-
// justified with the optional sats-glyph prefix.
val parseSatsPerCurrency(int price_int, std::string currency,
                         bool with_sats_symbol) {
  char price_buf[16];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const int32_t sats = btclock::SatsPerUnit(price_buf);
  const bool moscow =
      currency == "USD" && sats > 0 && sats < 100000;
  const std::string label = moscow ? "MSCW/TIME" : ("SATS/" + currency);

  const btclock::DigitLayout layout =
      btclock::ComputeMoscowLayout(sats, with_sats_symbol);

  val a = val::array();
  a.set(0, label);
  // The on-device sats glyph is a private-use codepoint in the Satoshi
  // Symbol font. For the HTML preview we substitute the ⚡ bolt as a
  // visible placeholder; layout positioning is what matters here.
  static const char* kSatsPlaceholder = "\xE2\x9A\xA1";  // U+26A1
  for (int i = 0; i < 6; ++i) {
    if (layout.is_sats[i]) {
      a.set(i + 1, std::string(kSatsPlaceholder));
    } else {
      char cell[2] = {layout.digits[i], '\0'};
      a.set(i + 1, std::string(cell));
    }
  }
  return a;
}

// parseBlockFees — "FEE/RATE" + integer sats/vB right-justified.
val parseBlockFees(int fee_sats_vb) {
  std::array<char, 6> d{};
  btclock::LayoutFeeRate<6>(fee_sats_vb, d);
  return DigitsToArray("FEE/RATE", d);
}

// The remaining three screens — halving countdown, market cap, bitcoin
// supply — only exist on worktree-agent-af32422c. Stub here so the HTML
// doesn't break; the cell content signals the gap.
val NotYetAvailable(const char* label) {
  val a = val::array();
  a.set(0, std::string(label));
  for (int i = 1; i < 7; ++i) {
    a.set(i, std::string("(wt-af3)"));
  }
  return a;
}

val parseHalvingCountdown(int, bool) { return NotYetAvailable("HALVING"); }
val parseMarketCap(int, int, std::string, bool) { return NotYetAvailable("MARKET/CAP"); }
val parseBitcoinSupply(int, bool, bool) { return NotYetAvailable("BTC/SUPPLY"); }

}  // namespace

EMSCRIPTEN_BINDINGS(btclock_idf_screens) {
  emscripten::function("parseBlockHeight", &parseBlockHeight);
  emscripten::function("parsePriceData", &parsePriceData);
  emscripten::function("parseSatsPerCurrency", &parseSatsPerCurrency);
  emscripten::function("parseBlockFees", &parseBlockFees);
  emscripten::function("parseHalvingCountdown", &parseHalvingCountdown);
  emscripten::function("parseMarketCap", &parseMarketCap);
  emscripten::function("parseBitcoinSupply", &parseBitcoinSupply);
}
