#include "screens/common.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace btclock {

void FormatDigits(uint32_t h, char* digits, size_t slots) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
  const size_t len = std::strlen(buf);
  const char* src = buf;
  size_t pad = 0;
  if (len > slots) {
    src = buf + (len - slots);
  } else {
    pad = slots - len;
  }
  for (size_t i = 0; i < slots; ++i) {
    digits[i] = (i < pad) ? ' ' : src[i - pad];
  }
}

int32_t SatsPerUnit(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p <= 0.0 || endp == price_str.c_str()) return -1;
  const double sats = 1e8 / p;
  if (sats > 4e9) return -1;
  return static_cast<int32_t>(sats + 0.5);
}

int32_t PriceInt(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p < 0.0 || endp == price_str.c_str()) return -1;
  if (p > 2e9) return -1;
  return static_cast<int32_t>(p + 0.5);
}

DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol) {
  DigitLayout l;
  if (sats < 0) return l;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
  const size_t len = std::strlen(buf);
  const size_t slots = 6;
  if (len >= slots) {
    const size_t start = len - slots;
    for (size_t i = 0; i < slots; ++i) l.digits[i] = buf[start + i];
    return l;
  }
  const size_t pad = slots - len;
  for (size_t i = 0; i < slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  if (use_symbol && pad > 0) {
    l.is_sats[pad - 1] = true;
    l.digits[pad - 1] = ' ';
  }
  return l;
}

}  // namespace btclock
