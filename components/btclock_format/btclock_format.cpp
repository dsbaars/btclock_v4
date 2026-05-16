// btclock_format — bodies for the panel-agnostic formatters declared
// in btclock_format.hpp. See the header for the rationale (component-
// shared with the future landscape variant).
//
// Bodies were lifted verbatim from main/screens/screen_math.cpp; the
// only behavioural change in this move is locality, not output.

#include "btclock_format/btclock_format.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace btclock {

std::string FormatZapAmount(const std::optional<int64_t>& amount_sats,
                            std::size_t max_int_cells) {
  if (!amount_sats || *amount_sats < 0) return "?";
  const int64_t v = *amount_sats;
  // Prefer the raw integer when it fits the panel-tail budget — readers
  // see "1000" / "10000" instead of "1.0K" / "10K". The budget is the
  // tail width without the sats glyph reserved; the caller-side layout
  // (ComputeZapLayout / ComputeNwcLayout / BuildLabelGlyphAmount) drops
  // the glyph automatically when the integer fills the tail.
  char int_buf[24];
  std::snprintf(int_buf, sizeof(int_buf), "%lld", static_cast<long long>(v));
  if (std::strlen(int_buf) <= max_int_cells) return int_buf;
  // Fall back to K / M / B suffix for values that don't fit. Uppercase
  // K matches FormatNumberWithSuffix below, so the BTC ticker, market
  // cap, supply, zap, and NWC paths share one suffix vocabulary.
  // Three-digit integer part: "12K", "123K". Two-digit: "12K". One-
  // digit: "1.2K" — the fractional digit gives a finer read at the
  // small end of each magnitude band.
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
    suffix = "K";
  }
  char buf[16];
  if (x >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(x + 0.5), suffix);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f%s", x, suffix);
  }
  return buf;
}

void FormatDigits64(uint64_t v, char* digits, std::size_t slots) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  const std::size_t len = std::strlen(buf);
  const char* src = buf;
  std::size_t pad = 0;
  if (len > slots) {
    src = buf + (len - slots);
  } else {
    pad = slots - len;
  }
  for (std::size_t i = 0; i < slots; ++i) {
    digits[i] = (i < pad) ? ' ' : src[i - pad];
  }
}

// Port of lib/btclock/utils.cpp::formatNumberWithSuffix — keep the
// branch order and decimal-packing logic identical, we test for
// byte-for-byte output against the old firmware via parity tests.
std::string FormatNumberWithSuffix(uint64_t num, int num_characters,
                                   bool mow_mode) {
  char result[24];
  constexpr long long kQuadrillion = 1000000000000000LL;
  constexpr long long kTrillion = 1000000000000LL;
  constexpr long long kBillion = 1000000000LL;
  constexpr long long kMillion = 1000000LL;
  constexpr long long kThousand = 1000LL;

  if (num == 0) {
    if (mow_mode) {
      std::snprintf(result, sizeof(result), "0M");
    } else {
      std::snprintf(result, sizeof(result), "0");
    }
    return result;
  }

  double num_d = static_cast<double>(num);
  const int num_digits = static_cast<int>(std::log10(num_d)) + 1;
  char suffix;

  if (static_cast<long long>(num) >= kQuadrillion || num_digits > 15) {
    num_d /= static_cast<double>(kQuadrillion);
    suffix = 'Q';
  } else if (static_cast<long long>(num) >= kTrillion || num_digits > 12) {
    num_d /= static_cast<double>(kTrillion);
    suffix = 'T';
  } else if (static_cast<long long>(num) >= kBillion || num_digits > 9) {
    num_d /= static_cast<double>(kBillion);
    suffix = 'B';
  } else if (static_cast<long long>(num) >= kMillion || num_digits > 6 ||
             (mow_mode && static_cast<long long>(num) >= kThousand)) {
    num_d /= static_cast<double>(kMillion);
    suffix = 'M';
  } else if (!mow_mode &&
             (static_cast<long long>(num) >= kThousand || num_digits > 3)) {
    num_d /= static_cast<double>(kThousand);
    suffix = 'K';
  } else if (!mow_mode) {
    std::snprintf(result, sizeof(result), "%llu",
                  static_cast<unsigned long long>(num));
    return result;
  } else {
    num_d /= static_cast<double>(kMillion);
    suffix = 'M';
  }

  int len;
  // gcc 15's libstdc++ implements std::to_string(double) on top of
  // std::to_chars<double>, which drags ~122 KiB of Ryu/Grisu rodata
  // tables into the link. We only need a fixed-format decimal string
  // here (the slicing below truncates at dot+N). Match what gcc 14's
  // to_string(double) emitted internally — vsnprintf("%.6f", v) — so
  // the visible output stays consistent with the firmware shipped
  // before the toolchain bump.
  char mow_buf[32];
  std::snprintf(mow_buf, sizeof(mow_buf), "%.6f", num_d);
  std::string mow_as_string = mow_buf;
  if (mow_mode) {
    // MOW truncates (never rounds) to preserve the at-time value.
    const std::size_t dot = mow_as_string.find('.');
    const std::size_t take =
        dot == std::string::npos ? mow_as_string.size() : dot + 2;
    len = std::snprintf(result, sizeof(result), "%s%c",
                        mow_as_string.substr(0, take).c_str(), suffix);
  } else {
    len = std::snprintf(result, sizeof(result), "%.0f%c", num_d, suffix);
  }

  if (len < num_characters) {
    const int rest_len =
        mow_mode ? num_characters - len : num_characters - len - 1;
    if (mow_mode) {
      const std::size_t dot = mow_as_string.find('.');
      const std::size_t take =
          dot == std::string::npos
              ? mow_as_string.size()
              : dot + 2 + static_cast<std::size_t>(rest_len);
      std::snprintf(result, sizeof(result), "%s%c",
                    mow_as_string.substr(0, take).c_str(), suffix);
    } else {
      std::snprintf(result, sizeof(result), "%.*f%c", rest_len, num_d, suffix);
    }
  }

  return result;
}

HalvingTimeBreakdown HalvingCountdownBreakdown(uint32_t block_height) {
  HalvingTimeBreakdown out;
  // 10-minute blocks → minutes-until-halving drives the cascade. Old
  // firmware used `floor(minutes/525600)` for years, same here.
  const uint32_t blocks_to_halving = HalvingCountdown(block_height);
  const uint64_t minutes = static_cast<uint64_t>(blocks_to_halving) * 10ULL;
  out.years = static_cast<uint32_t>(minutes / 525600ULL);
  uint64_t remaining = minutes - static_cast<uint64_t>(out.years) * 525600ULL;
  out.days = static_cast<uint32_t>(remaining / 1440ULL);
  remaining -= static_cast<uint64_t>(out.days) * 1440ULL;
  out.hours = static_cast<uint32_t>(remaining / 60ULL);
  remaining -= static_cast<uint64_t>(out.hours) * 60ULL;
  out.minutes = static_cast<uint32_t>(remaining);
  return out;
}

MiningPoolHashrateLayout LayoutMiningPoolHashrate(
    const std::string& hashrate_raw, unsigned int max_chars) {
  // "No data yet" / parse-error path matches old firmware
  // parseHashrateString: label H/S, value "0".
  MiningPoolHashrateLayout out{"0", "H/S"};
  if (hashrate_raw.empty() || hashrate_raw == "0") return out;
  if (!std::isdigit(static_cast<unsigned char>(hashrate_raw[0]))) return out;

  // Unit ladder keyed off the raw string's digit count — the pollers
  // land integer H/s as reported by the pool API. Thresholds mirror the
  // old firmware (hashrate.length() > 21 → ZH/S, etc.).
  std::size_t suffix_len = 0;
  const std::size_t n = hashrate_raw.size();
  if (n > 21) {
    out.unit = "ZH/S";
    suffix_len = 21;
  } else if (n > 18) {
    out.unit = "EH/S";
    suffix_len = 18;
  } else if (n > 15) {
    out.unit = "PH/S";
    suffix_len = 15;
  } else if (n > 12) {
    out.unit = "TH/S";
    suffix_len = 12;
  } else if (n > 9) {
    out.unit = "GH/S";
    suffix_len = 9;
  } else if (n > 6) {
    out.unit = "MH/S";
    suffix_len = 6;
  } else if (n > 3) {
    out.unit = "KH/S";
    suffix_len = 3;
  } else {
    out.unit = "H/S";
    suffix_len = 0;
  }

  char* endp = nullptr;
  double value = std::strtod(hashrate_raw.c_str(), &endp);
  if (endp == hashrate_raw.c_str()) {
    return MiningPoolHashrateLayout{"0", "H/S"};
  }
  value /= std::pow(10.0, static_cast<double>(suffix_len));

  // Decimal-count decision: integer digits eat the width budget first;
  // the rest go to the fractional part. A single char left of the dot
  // gets (max_chars-1) decimals; values that fill max_chars digits
  // before the dot get no decimals (just round).
  char buf[32];
  const int integer_len =
      static_cast<int>(std::to_string(static_cast<long long>(value)).size());
  const int remaining = static_cast<int>(max_chars) - integer_len;
  if (remaining <= 0) {
    std::snprintf(buf, sizeof(buf), "%.0f", value);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*f", remaining - 1, value);
  }
  std::string s(buf);
  // Strip trailing zeros after a decimal point — "1.300" → "1.3",
  // and "1.000" → "1". Avoids wasting digit slots on noise.
  if (s.find('.') != std::string::npos) {
    const auto nz = s.find_last_not_of('0');
    s = s.substr(0, nz + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  out.value = std::move(s);
  return out;
}

MiningPoolEarningsLayout LayoutMiningPoolEarnings(int64_t daily_sats) {
  MiningPoolEarningsLayout out;
  if (daily_sats < 0) return out;
  out.valid = true;
  out.unit_label = "SATS";

  if (daily_sats >= 100000000LL) {
    // A whale mining ≥ 1 BTC/day. Drop to whole-BTC units with a BTC label.
    const int64_t btc = daily_sats / 100000000LL;
    out.value = std::to_string(btc);
    out.unit_label = "BTC";
    return out;
  }
  if (daily_sats >= 10000000LL) {
    // 10M..99.9M sats → "NN.NM" (one decimal).
    const int64_t tens_of_millions = daily_sats / 1000000LL;  // e.g. 12
    const int64_t hundred_thousands_digit =
        (daily_sats / 100000LL) % 10LL;  // 0..9
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lld.%lldM",
                  static_cast<long long>(tens_of_millions),
                  static_cast<long long>(hundred_thousands_digit));
    out.value = buf;
    return out;
  }
  if (daily_sats >= 1000000LL) {
    // 1M..9.99M sats → "N.NNM" (two decimals).
    const int64_t millions = daily_sats / 1000000LL;  // 1..9
    const int64_t hundreds_of_thousands =
        (daily_sats / 10000LL) % 100LL;  // 0..99
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lld.%02lldM",
                  static_cast<long long>(millions),
                  static_cast<long long>(hundreds_of_thousands));
    out.value = buf;
    return out;
  }
  if (daily_sats >= 100000LL) {
    // 100K..999K sats → "NNNK" (no decimals).
    const int64_t k = daily_sats / 1000LL;
    out.value = std::to_string(k) + "K";
    return out;
  }
  if (daily_sats >= 10000LL) {
    // 10K..99.9K sats → "NN.NK" (one decimal).
    const int64_t tens_of_k = daily_sats / 1000LL;              // e.g. 12
    const int64_t hundred_digit = (daily_sats / 100LL) % 10LL;  // 0..9
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lld.%lldK",
                  static_cast<long long>(tens_of_k),
                  static_cast<long long>(hundred_digit));
    out.value = buf;
    return out;
  }
  // Pleb-miner tier: ≤4-digit sats fit verbatim.
  out.value = std::to_string(daily_sats);
  return out;
}

}  // namespace btclock
