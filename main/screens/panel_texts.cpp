#include "screens/panel_texts.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "screens/fee_rate_layout.hpp"
#include "screens/screen_math.hpp"

namespace btclock {
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

// UTF-8 currency symbol for the given ISO code, or "" if none available.
// Mirrors common.cpp's CurrencySymbolUtf8 (kept local for header purity).
const char* CurrencySymbolLocal(const std::string& ccy) {
  if (ccy == "USD") return "$";
  if (ccy == "EUR") return "\xE2\x82\xAC";
  if (ccy == "GBP") return "\xC2\xA3";
  if (ccy == "JPY") return "\xC2\xA5";
  return "";
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

std::vector<std::string> BuildHalving(uint32_t h, std::size_t n_panels) {
  std::vector<std::string> out;
  out.reserve(n_panels);
  const uint32_t rem = HalvingCountdown(h);
  out.emplace_back("HAL/VING");
  const std::size_t digit_slots = n_panels - 1;
  std::vector<char> digits(digit_slots);
  FormatDigits64(static_cast<uint64_t>(rem), digits.data(), digit_slots);
  AppendDigits(digits.data(), digit_slots, out);
  return out;
}

// Right-pad a FormatNumberWithSuffix-style string across n_panels slots,
// mirroring the parity helper's layout: slot 0 is the caller-supplied
// label, slot 1..n_panels-1 = s[1..n_panels-1] after left-space-padding
// to n_panels chars. s[0] is consumed by the padding and discarded.
std::vector<std::string> EmitBigCharsFrame(std::string label, std::string s,
                                           std::size_t n_panels) {
  std::vector<std::string> out;
  out.reserve(n_panels);
  out.emplace_back(std::move(label));
  if (s.size() < n_panels) s.insert(s.begin(), n_panels - s.size(), ' ');
  for (std::size_t i = 1; i < n_panels; ++i) {
    out.push_back(CharSlot(s[i]));
  }
  return out;
}

std::vector<std::string> BuildBitcoinSupply(uint32_t h, std::size_t n_panels) {
  // BigChars suffix form ("19.9M") — matches the renderer's default. The
  // pre-suffix integer-digit path silently truncates supply (19.7M → six
  // low digits), see btclock_v3_fci-0v9 for the parity-layer test that
  // pins this layout. showPercentage variant lives under btclock_v3_fci-33e.
  const uint64_t supply = SupplyAtBlock(h);
  std::string s = FormatNumberWithSuffix(supply,
                                         static_cast<int>(n_panels) - 2);
  return EmitBigCharsFrame("BTC/SUPPLY", std::move(s), n_panels);
}

std::vector<std::string> BuildMarketCap(uint32_t h, const std::string& price,
                                        const std::string& currency,
                                        std::size_t n_panels) {
  // BigChars suffix form prefixed by the currency glyph. Label in slot 0
  // is "<CCY>/MCAP"; slots 1..n-1 carry " <SYM>NNNX" after left-space-pad.
  // Parity source: test_datahandler_parity.cpp::RenderMarketCapBigChars.
  const int32_t pi = PriceIntLocal(price);
  const uint64_t cap = (pi < 0) ? 0 : MarketCap(static_cast<uint32_t>(pi), h);
  const char* sym = CurrencySymbolLocal(currency);
  std::string glyph = (sym && *sym) ? std::string(sym) : currency;
  std::string s = glyph + FormatNumberWithSuffix(
                              cap, static_cast<int>(n_panels) - 2);
  return EmitBigCharsFrame(currency + "/MCAP", std::move(s), n_panels);
}

std::vector<std::string> BuildMoscowTime(const std::string& currency,
                                         const std::string& price,
                                         std::size_t n_panels) {
  // Label rule mirrors RenderMoscowTimeScreen: USD with sats in the
  // classic Moscow-time range (0 < sats < 100_000) gets "MSCW/TIME";
  // everything else (other currencies, out-of-range sats) gets
  // "SATS/<CCY>". The sats-glyph panel (one slot before the first
  // digit) is emitted as "STS" — same token the old firmware's
  // parseSatsPerCurrency uses for its sats marker.
  std::vector<std::string> out;
  out.reserve(n_panels);
  const int32_t sats = SatsPerUnitLocal(price);
  const bool moscow =
      currency == "USD" && sats > 0 && sats < 100000;
  out.emplace_back(moscow ? std::string("MSCW/TIME")
                          : ("SATS/" + currency));

  // Exactly the same 6-digit layout the renderer computes via
  // ComputeMoscowLayout. kDigitPanels is always 6 in the renderer
  // (hard-coded), so slots 1..6 carry digits and slot 7 (when present)
  // is left blank. That matches the on-screen state: the 8-panel
  // variant's trailing panel is blank.
  constexpr std::size_t kDigitSlots = 6;
  std::array<char, kDigitSlots> digits{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, kDigitSlots> is_sats{};
  if (sats >= 0) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
    const std::size_t len = std::strlen(buf);
    if (len >= kDigitSlots) {
      const std::size_t start = len - kDigitSlots;
      for (std::size_t i = 0; i < kDigitSlots; ++i) digits[i] = buf[start + i];
    } else {
      const std::size_t pad = kDigitSlots - len;
      for (std::size_t i = 0; i < kDigitSlots; ++i) {
        digits[i] = (i < pad) ? ' ' : buf[i - pad];
      }
      if (pad > 0) {
        is_sats[pad - 1] = true;
        digits[pad - 1] = ' ';
      }
    }
  }

  for (std::size_t i = 0; i < kDigitSlots && out.size() < n_panels; ++i) {
    if (is_sats[i]) out.emplace_back("STS");
    else out.push_back(CharSlot(digits[i]));
  }
  while (out.size() < n_panels) out.emplace_back();
  return out;
}

std::vector<std::string> BuildBtcPrice(const std::string& currency,
                                       const std::string& price,
                                       std::size_t n_panels) {
  // Panel 0 = "BTC/<CCY>". Digits 1..N-1 right-justified; the UTF-8
  // currency symbol (if known) lives one slot before the first digit
  // iff there is a blank to place it in. On overflow the symbol panel
  // is dropped — matches RenderBtcPriceScreen exactly.
  std::vector<std::string> out;
  out.reserve(n_panels);
  out.emplace_back("BTC/" + currency);
  const std::size_t digit_slots = n_panels - 1;
  std::vector<char> digits(digit_slots, ' ');
  std::vector<bool> is_sym(digit_slots, false);
  const int32_t pi = PriceIntLocal(price);
  const int32_t vv = pi < 0 ? 0 : pi;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(vv));
  const std::size_t len = std::strlen(buf);
  const char* sym = CurrencySymbolLocal(currency);
  const bool use_symbol = sym[0] != '\0';
  if (len >= digit_slots) {
    for (std::size_t i = 0; i < digit_slots; ++i)
      digits[i] = buf[len - digit_slots + i];
  } else {
    const std::size_t pad = digit_slots - len;
    for (std::size_t i = pad; i < digit_slots; ++i) digits[i] = buf[i - pad];
    if (use_symbol && pad > 0) is_sym[pad - 1] = true;
  }
  for (std::size_t i = 0; i < digit_slots; ++i) {
    if (is_sym[i]) out.emplace_back(sym);
    else out.push_back(CharSlot(digits[i]));
  }
  return out;
}

std::vector<std::string> BuildFeeRate(
    const std::optional<double>& fee_opt, std::size_t n_panels) {
  // Panel 0 = "FEE/RATE", panels 1..N-2 = digits, panel N-1 = "sat/vB".
  // Falls back to blank digits when `fee_opt` is empty or negative —
  // mirrors the renderer's "not-yet-received" state.
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

std::vector<std::string> BuildClock(bool valid, int hour, int minute,
                                    int mday, int month,
                                    std::size_t n_panels) {
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
      ComputeClockLayout(valid, hour, minute, digit_slots);
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
      return BuildHalving(in.block_height.value_or(0), n_panels);
    case ScreenType::kBitcoinSupply:
      return BuildBitcoinSupply(in.block_height.value_or(0), n_panels);
    case ScreenType::kMarketCap:
      return BuildMarketCap(in.block_height.value_or(0), in.price,
                            in.currency, n_panels);
    case ScreenType::kMoscowTime:
      return BuildMoscowTime(in.currency, in.price, n_panels);
    case ScreenType::kBtcPrice:
      return BuildBtcPrice(in.currency, in.price, n_panels);
    case ScreenType::kBlockFeeRate:
      return BuildFeeRate(in.block_fee_sats_vb, n_panels);
    case ScreenType::kClock:
      return BuildClock(in.clock_valid, in.hour, in.minute, in.mday,
                        in.month, n_panels);
  }
  // Exhaustive switch above; fallback only on unreachable enum values.
  return std::vector<std::string>(n_panels);
}

}  // namespace btclock
