#include "screens/screen_math.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace btclock {

void FormatDigits64(uint64_t v, char* digits, std::size_t slots) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu",
                static_cast<unsigned long long>(v));
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

std::vector<std::string> SmallCharsGroups(uint64_t value,
                                          const std::string& ccy_cell,
                                          std::size_t slots) {
  std::vector<std::string> out(slots);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu",
                static_cast<unsigned long long>(value));
  std::string s = buf;
  const std::size_t len = s.size();
  const std::size_t leading = (3 - len % 3) % 3;
  s.insert(s.begin(), leading, ' ');
  const std::size_t groups = (len + leading) / 3;
  // Old-firmware parity: when the groups fit, put the currency
  // separator one slot ahead of the first digit group; earlier slots
  // stay empty. Overflow (groups >= slots) drops the separator and
  // right-aligns as many groups as will fit — matching old firmware.
  if (groups + 1 <= slots) {
    const std::size_t sep = slots - groups - 1;
    out[sep] = ccy_cell.empty() ? std::string(" ") : ccy_cell;
    for (std::size_t i = 0; i < groups; ++i) {
      out[slots - groups + i] = s.substr(i * 3, 3);
    }
  } else {
    const std::size_t keep = slots;
    const std::size_t excess = groups - keep;
    for (std::size_t i = 0; i < keep; ++i) {
      out[i] = s.substr((excess + i) * 3, 3);
    }
  }
  return out;
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
  std::string mow_as_string = std::to_string(num_d);
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
      std::snprintf(result, sizeof(result), "%.*f%c", rest_len, num_d,
                    suffix);
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
    out.unit = "ZH/S"; suffix_len = 21;
  } else if (n > 18) {
    out.unit = "EH/S"; suffix_len = 18;
  } else if (n > 15) {
    out.unit = "PH/S"; suffix_len = 15;
  } else if (n > 12) {
    out.unit = "TH/S"; suffix_len = 12;
  } else if (n > 9) {
    out.unit = "GH/S"; suffix_len = 9;
  } else if (n > 6) {
    out.unit = "MH/S"; suffix_len = 6;
  } else if (n > 3) {
    out.unit = "KH/S"; suffix_len = 3;
  } else {
    out.unit = "H/S"; suffix_len = 0;
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
    const int64_t tens_of_millions = daily_sats / 1000000LL;      // e.g. 12
    const int64_t hundred_thousands_digit =
        (daily_sats / 100000LL) % 10LL;                            // 0..9
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lld.%lldM",
                  static_cast<long long>(tens_of_millions),
                  static_cast<long long>(hundred_thousands_digit));
    out.value = buf;
    return out;
  }
  if (daily_sats >= 1000000LL) {
    // 1M..9.99M sats → "N.NNM" (two decimals).
    const int64_t millions = daily_sats / 1000000LL;               // 1..9
    const int64_t hundreds_of_thousands =
        (daily_sats / 10000LL) % 100LL;                            // 0..99
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
    const int64_t tens_of_k = daily_sats / 1000LL;                 // e.g. 12
    const int64_t hundred_digit = (daily_sats / 100LL) % 10LL;     // 0..9
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

ClockLayout ComputeClockLayout(bool valid, int hour, int minute,
                               std::size_t digit_panels) {
  ClockLayout l;
  for (std::size_t i = 0; i < sizeof(l.digits); ++i) l.digits[i] = ' ';
  if (!valid || digit_panels < 5 || digit_panels > sizeof(l.digits)) {
    return l;
  }
  const int h = hour < 0 ? 0 : (hour > 23 ? 23 : hour);
  const int m = minute < 0 ? 0 : (minute > 59 ? 59 : minute);
  const std::size_t base = digit_panels - 5;
  l.digits[base + 0] = static_cast<char>('0' + (h / 10));
  l.digits[base + 1] = static_cast<char>('0' + (h % 10));
  l.digits[base + 2] = ':';
  l.digits[base + 3] = static_cast<char>('0' + (m / 10));
  l.digits[base + 4] = static_cast<char>('0' + (m % 10));
  return l;
}

}  // namespace btclock
