#include "screens/screen_math.hpp"

#include <cstdio>
#include <cstring>

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
