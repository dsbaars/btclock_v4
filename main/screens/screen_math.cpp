#include "screens/screen_math.hpp"

#include <cmath>
#include <cstdio>
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
