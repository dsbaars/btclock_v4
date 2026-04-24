// Pure-logic helper: parse the compiler's __DATE__ / __TIME__ pair into
// a Unix timestamp (seconds since 1970-01-01 UTC).
//
// Split out of the webserver transport layer so the host test suite can
// lock the parser's behaviour (leap years, single-digit days, month
// abbreviations) without linking ESP-IDF. Used by control_server.cpp to
// populate DeviceContext::last_build_time_unix.
//
// __DATE__ format is `MMM DD YYYY` — month abbreviation (Jan/Feb/…),
// day (space-padded when single-digit), year. __TIME__ format is
// `HH:MM:SS` with zero-padding. Both values are treated as UTC so the
// emitted timestamp is independent of the build host's local time.

#pragma once

#include <cstdint>
#include <string_view>

namespace btclock {
namespace settings {

// Parse a `__DATE__` + `__TIME__` pair into a Unix seconds value.
// Returns 0 on any format mismatch — the caller is expected to treat 0
// as "build time unknown" and omit the field from the response.
//
// The function intentionally does NOT consult the build host's locale
// or TZ environment; it performs the conversion arithmetically, using
// the civil-to-POSIX formula (Howard Hinnant's date algorithm) so the
// result is reproducible across hosts.
int64_t ParseCompilerBuildTimeUnix(std::string_view date_str,
                                   std::string_view time_str);

}  // namespace settings
}  // namespace btclock
