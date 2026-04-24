// Implementation of ParseCompilerBuildTimeUnix. Kept standalone so the
// host test suite links it without cJSON / PrefsReader overhead.

#include "settings/build_time.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace btclock {
namespace settings {
namespace {

// Three-letter month abbreviation -> 1-based month index. Compiler
// emits them in English regardless of locale so a fixed lookup suffices.
int MonthFromAbbrev(std::string_view s) {
  if (s.size() != 3) return 0;
  static const char* kMonths[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  for (int i = 0; i < 12; ++i) {
    if (s[0] == kMonths[i][0] && s[1] == kMonths[i][1] &&
        s[2] == kMonths[i][2]) {
      return i + 1;
    }
  }
  return 0;
}

// Parse a decimal integer of at most `max_digits` from `s`, starting at
// `pos`. On success advances `pos` past the digits and writes the value
// into `out`; on failure leaves both untouched and returns false.
// Leading ASCII spaces are tolerated (they pad single-digit day fields
// in __DATE__).
bool ParseIntAt(std::string_view s, size_t& pos, int max_digits, int& out) {
  int v = 0;
  size_t i = pos;
  while (i < s.size() && s[i] == ' ') ++i;
  size_t start = i;
  while (i < s.size() && i - start < static_cast<size_t>(max_digits) &&
         std::isdigit(static_cast<unsigned char>(s[i]))) {
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  if (i == start) return false;
  pos = i;
  out = v;
  return true;
}

// Days from 1970-01-01 to the civil date (Y, M, D) using Hinnant's
// algorithm. Works for any Gregorian date; the build-time values we
// feed are comfortably inside the arithmetic range.
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 +
         static_cast<int64_t>(doe) - 719468;
}

}  // namespace

int64_t ParseCompilerBuildTimeUnix(std::string_view date_str,
                                   std::string_view time_str) {
  // __DATE__ is always 11 chars ("MMM DD YYYY") and __TIME__ is 8
  // ("HH:MM:SS"), but the compiler is allowed to emit them otherwise.
  // Parse robustly instead of asserting lengths.
  if (date_str.size() < 11 || time_str.size() < 8) return 0;

  const int month = MonthFromAbbrev(date_str.substr(0, 3));
  if (month < 1 || month > 12) return 0;
  if (date_str[3] != ' ') return 0;

  size_t pos = 4;
  int day = 0;
  if (!ParseIntAt(date_str, pos, 2, day)) return 0;
  if (day < 1 || day > 31) return 0;
  if (pos >= date_str.size() || date_str[pos] != ' ') return 0;
  ++pos;
  int year = 0;
  if (!ParseIntAt(date_str, pos, 4, year)) return 0;
  if (year < 1970 || year > 9999) return 0;

  pos = 0;
  int hour = 0;
  if (!ParseIntAt(time_str, pos, 2, hour)) return 0;
  if (pos >= time_str.size() || time_str[pos] != ':') return 0;
  ++pos;
  int minute = 0;
  if (!ParseIntAt(time_str, pos, 2, minute)) return 0;
  if (pos >= time_str.size() || time_str[pos] != ':') return 0;
  ++pos;
  int second = 0;
  if (!ParseIntAt(time_str, pos, 2, second)) return 0;

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60) return 0;  // allow leap second

  const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month),
                                     static_cast<unsigned>(day));
  return days * 86400 + static_cast<int64_t>(hour) * 3600 +
         static_cast<int64_t>(minute) * 60 + second;
}

}  // namespace settings
}  // namespace btclock
