// Pure-logic AxeOS /api/system/info parser.
//
// Split from the IDF-heavy poller so host tests can exercise the JSON
// shape without mocking esp_http_client. Same separation pattern as the
// mining_pool_* components.
//
// AxeOS response shape (current Gamma firmware, 2024-10):
//   { "hashRate":        <number, GH/s>,
//     "bestDiff":        "15.6M" | <number>,
//     "sharesAccepted":  <int>,
//     "temp":            <number, °C>, … }
//
// We also accept `temperature` as an alias for `temp` (older forks).
// Missing fields are tolerated — the returned struct's optionals stay
// nullopt so the snapshot keeps its previous value.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace btclock {
namespace bitaxe {

struct ParsedStats {
  std::optional<double>      hashrate_ghs;
  std::optional<std::string> best_diff;      // canonicalised "<val><unit>"
  std::optional<double>      temperature_c;
  std::optional<int32_t>     shares_accepted;
};

// Canonicalise an AxeOS-supplied bestDiff value into a string the
// renderer can paint verbatim. AxeOS returns either:
//   - a human string like "15.6M" (older firmware, kept unchanged);
//   - a raw number (newer firmware, compressed to "<val><K/M/G/T/P>").
// Pure-logic so panel_texts and the renderer see byte-identical output.
std::string FormatBestDiff(double raw);

// Parse a NUL-terminated JSON body. Returns true on a parse we could
// read fields from (even if some optionals stay nullopt). Returns false
// only for malformed JSON or non-object roots — callers log+skip in
// that case, keeping the last good snapshot.
bool Parse(const char* body, ParsedStats& out);

}  // namespace bitaxe
}  // namespace btclock
