#include "screens/screen_math.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace btclock {

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

ClockLayout ComputeClockLayout(bool valid, int hour, int minute,
                               std::size_t digit_panels,
                               bool hide_leading_zero) {
  ClockLayout l;
  for (std::size_t i = 0; i < sizeof(l.digits); ++i) l.digits[i] = ' ';
  if (!valid || digit_panels < 5 || digit_panels > sizeof(l.digits)) {
    return l;
  }
  const int h = hour < 0 ? 0 : (hour > 23 ? 23 : hour);
  const int m = minute < 0 ? 0 : (minute > 59 ? 59 : minute);
  const std::size_t base = digit_panels - 5;
  const char tens = static_cast<char>('0' + (h / 10));
  // hide_leading_zero blanks the tens-of-hours cell for 0..9 so the
  // display reads " 7:00" instead of "07:00"; two-digit hours (10..23)
  // are unaffected. The colon and minute digits keep their slots so
  // layout width stays constant — only the glyph in the leading slot
  // changes. Matches the user-visible "7:00" example in the setting
  // description: minute stays two-digit ("7:05", not "7:5").
  l.digits[base + 0] = (hide_leading_zero && h < 10) ? ' ' : tens;
  l.digits[base + 1] = static_cast<char>('0' + (h % 10));
  l.digits[base + 2] = ':';
  l.digits[base + 3] = static_cast<char>('0' + (m / 10));
  l.digits[base + 4] = static_cast<char>('0' + (m % 10));
  return l;
}

}  // namespace btclock
