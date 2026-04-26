// Host tests for the timezone IANA->POSIX lookup table. Exercises the
// constexpr PosixForIana() helper from components/timezone; the NVS +
// setenv wrapper (SetTimezoneByName, InitFromNvs) is NOT tested here —
// it pulls in the prefs component, which needs nvs_flash.

#include <string_view>

#include "doctest.h"
#include "timezone/timezone.hpp"

using btclock::timezone::kTzTable;
using btclock::timezone::PosixForIana;

TEST_CASE("timezone: well-known IANA zones round-trip to POSIX") {
  // Handpicked zones covering the five major continents + UTC. If the
  // upstream tz database ever redefines one of these, that change
  // needs to land in include/timezone_data.hpp first and then be
  // regenerated into tz_table_generated.hpp via generate_tz_table.py.
  CHECK(PosixForIana("UTC") == std::string_view{"UTC0"});
  CHECK(PosixForIana("Europe/Amsterdam") ==
        std::string_view{"CET-1CEST,M3.5.0,M10.5.0/3"});
  CHECK(PosixForIana("America/New_York") ==
        std::string_view{"EST5EDT,M3.2.0,M11.1.0"});
  CHECK(PosixForIana("Asia/Tokyo") == std::string_view{"JST-9"});
  CHECK(PosixForIana("Pacific/Auckland") ==
        std::string_view{"NZST-12NZDT,M9.5.0,M4.1.0/3"});
}

TEST_CASE("timezone: Europe/Brussels resolves to a DST-aware POSIX string") {
  // Regression for the "tzString written to settings NVS namespace,
  // never applied" live-device bug. The IANA name we ship in
  // GET /api/settings must round-trip to a POSIX string that encodes
  // the DST window — a fixed-offset fallback (e.g. "CET-1") would drift
  // an hour in summer. Brussels shares the CET cluster with
  // Amsterdam/Berlin/Paris, so verify the canonical expansion.
  const std::string_view posix = PosixForIana("Europe/Brussels");
  REQUIRE(!posix.empty());
  CHECK(posix == std::string_view{"CET-1CEST,M3.5.0,M10.5.0/3"});
}

TEST_CASE("timezone: unknown zones return an empty view") {
  CHECK(PosixForIana("").empty());
  CHECK(PosixForIana("Not/A_Real_Zone").empty());
  // Leading/trailing whitespace is *not* trimmed — callers are
  // responsible for sending canonical IANA IDs.
  CHECK(PosixForIana(" UTC").empty());
  CHECK(PosixForIana("UTC ").empty());
}

TEST_CASE("timezone: table size matches upstream firmware (484 zones)") {
  // Guards against accidental truncation when regenerating.
  CHECK(kTzTable.size() == 484);
}

TEST_CASE("timezone: every entry has a non-empty POSIX string") {
  for (const auto& entry : kTzTable) {
    REQUIRE_MESSAGE(!entry.first.empty(), "empty IANA name in table");
    REQUIRE_MESSAGE(!entry.second.empty(),
                    "empty POSIX TZ for " << std::string(entry.first));
  }
}

TEST_CASE("timezone: table is sorted by IANA name (enables future bsearch)") {
  std::string_view prev;
  for (const auto& entry : kTzTable) {
    if (!prev.empty()) {
      CHECK(prev < entry.first);
    }
    prev = entry.first;
  }
}
