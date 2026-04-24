// Host tests for settings::ParseCompilerBuildTimeUnix. Pins the
// behaviour of the __DATE__/__TIME__ -> Unix seconds parser so the
// /api/settings lastBuildTime field stays lossless across compiler
// versions and host locales.

#include "doctest.h"

#include "settings/build_time.hpp"

#include <cstdint>

using btclock::settings::ParseCompilerBuildTimeUnix;

TEST_CASE("ParseCompilerBuildTimeUnix epoch boundary") {
  // 1970-01-01T00:00:00Z -> 0.
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 1970", "00:00:00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix round number") {
  // 2020-01-01T00:00:00Z -> 1577836800 (per `date -u -d @1577836800`).
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 2020", "00:00:00") == 1577836800);
}

TEST_CASE("ParseCompilerBuildTimeUnix single-digit day is space-padded") {
  // GCC emits __DATE__ with a leading space when the day is 1-9.
  CHECK(ParseCompilerBuildTimeUnix("Apr  3 2024", "12:34:56") ==
        ParseCompilerBuildTimeUnix("Apr 03 2024", "12:34:56"));
}

TEST_CASE("ParseCompilerBuildTimeUnix handles all months") {
  // 2024 is a leap year — February 29 is valid. Pick one timestamp
  // per month and compare to `date -u -d 'YYYY-MM-DD 00:00:00' +%s`.
  struct Case {
    const char* date;
    int64_t expected;
  };
  const Case cases[] = {
      {"Jan  1 2024", 1704067200},
      {"Feb 29 2024", 1709164800},
      {"Mar 15 2024", 1710460800},
      {"Apr  1 2024", 1711929600},
      {"May 31 2024", 1717113600},
      {"Jun 15 2024", 1718409600},
      {"Jul  4 2024", 1720051200},
      {"Aug 20 2024", 1724112000},
      {"Sep  9 2024", 1725840000},
      {"Oct 31 2024", 1730332800},
      {"Nov 11 2024", 1731283200},
      {"Dec 25 2024", 1735084800},
  };
  for (const auto& c : cases) {
    CHECK(ParseCompilerBuildTimeUnix(c.date, "00:00:00") == c.expected);
  }
}

TEST_CASE("ParseCompilerBuildTimeUnix includes hours/minutes/seconds") {
  // 2026-04-24T15:30:45Z -> 1777044645.
  CHECK(ParseCompilerBuildTimeUnix("Apr 24 2026", "15:30:45") == 1777044645);
}

TEST_CASE("ParseCompilerBuildTimeUnix treats value as UTC regardless of host") {
  // This test would trip if the implementation used mktime/localtime on
  // a host with a non-UTC timezone; the civil-to-POSIX formula is
  // locale-free so we get the same answer everywhere.
  const int64_t a = ParseCompilerBuildTimeUnix("Jul  1 2025", "12:00:00");
  CHECK(a == 1751371200);
}

TEST_CASE("ParseCompilerBuildTimeUnix rejects unknown month") {
  CHECK(ParseCompilerBuildTimeUnix("Foo  1 2024", "00:00:00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix rejects wrong separator") {
  CHECK(ParseCompilerBuildTimeUnix("Jan-01 2024", "00:00:00") == 0);
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 2024", "00-00-00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix rejects out-of-range time fields") {
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 2024", "25:00:00") == 0);
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 2024", "00:60:00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix rejects short inputs") {
  CHECK(ParseCompilerBuildTimeUnix("", "") == 0);
  CHECK(ParseCompilerBuildTimeUnix("short", "00:00:00") == 0);
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 2024", "00:00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix rejects pre-1970 years") {
  // The encoder is allowed to produce anything >=1970; earlier dates
  // indicate a corrupt or stub __DATE__ and should fall back to 0.
  CHECK(ParseCompilerBuildTimeUnix("Jan  1 1969", "00:00:00") == 0);
}

TEST_CASE("ParseCompilerBuildTimeUnix allows leap second") {
  // POSIX mktime() accepts second=60 for transient leap seconds; the
  // parser inherits that leniency rather than rejecting what newlib
  // would have converted cleanly.
  CHECK(ParseCompilerBuildTimeUnix("Jun 30 2015", "23:59:60") != 0);
}
