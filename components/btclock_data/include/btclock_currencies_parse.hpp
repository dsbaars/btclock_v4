// Pure helper: parse the JSON array returned by `/api/v2/currencies`.
//
// The upstream shape is a top-level string array, e.g.
//   ["USD","AUD","GBP","JPY","EUR","CAD","CHF"]
//
// Split into its own TU (no esp_http_client / mbedtls deps) so host
// tests can pin the parsing surface and fuzz easily.

#pragma once

#include <string>
#include <vector>

namespace btclock {

// Returns the list of ISO-style 3-letter currency codes from `body`.
//
// Validation rules — defensive against an upstream that grew junk
// entries or a captive portal serving HTML:
//   * top-level token must be a JSON array
//   * each element must be a JSON string of exactly 3 characters
//   * each character must be ASCII A-Z (lowercased input is rejected,
//     not coerced — a non-uppercase code suggests the wrong endpoint)
//   * duplicates are dropped, preserving first occurrence
//
// Returns an empty vector when the body is not a JSON array or every
// entry was rejected. The caller treats empty as "fetch failed; keep
// the static catalogue."
std::vector<std::string> ParseCurrenciesJson(const char* body,
                                             std::size_t body_len);

inline std::vector<std::string> ParseCurrenciesJson(const std::string& body) {
  return ParseCurrenciesJson(body.data(), body.size());
}

}  // namespace btclock
